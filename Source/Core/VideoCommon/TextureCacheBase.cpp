// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include "Common/MemoryUtil.h"

#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/HiresTextures.h"
#include "VideoCommon/RenderBase.h"
#include "Common/FileUtil.h"

#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/TextureUtil.h"
#include "VideoCommon/Debugger.h"
#include "Core/ConfigManager.h"
#include "Core/HW/Memmap.h"

TextureCache *g_texture_cache;
GC_ALIGNED16(u8 *TextureCache::temp) = nullptr;
size_t TextureCache::temp_size;
u32 TextureCache::s_prev_tlut_address = 0;
u64 TextureCache::s_prev_tlut_hash = 0;
u32 TextureCache::s_prev_tlut_size = 0;
TextureCache::TexCache TextureCache::textures;
TextureCache::TexPool  TextureCache::texture_pool;
// Pool to save hires textures to avoid unnesessary reloads
TextureCache::HiresTexPool TextureCache::hires_texture_pool;
TextureCache::TCacheEntryBase* TextureCache::bound_textures[8];


TextureCache::BackupConfig TextureCache::backup_config;
// Small counter to track the amount of memory currently used on the gpu
size_t TextureCache::texture_pool_memory_usage = 0;

bool invalidate_texture_cache_requested;

TextureCache::TCacheEntryBase::~TCacheEntryBase()
{
}

void TextureCache::CheckTempSize(size_t required_size)
{
	if (required_size <= temp_size)
		return;

	temp_size = required_size;
	FreeAlignedMemory(temp);
	temp = (u8*)AllocateAlignedMemory(temp_size, 16);
}

TextureCache::TextureCache()
{
	temp_size = 2048 * 2048 * 4;
	if (!TextureCache::temp)
		TextureCache::temp = (u8*)AllocateAlignedMemory(temp_size, 16);

	TexDecoder_SetTexFmtOverlayOptions(g_ActiveConfig.bTexFmtOverlayEnable, g_ActiveConfig.bTexFmtOverlayCenter);

	if (g_ActiveConfig.bHiresTextures && !g_ActiveConfig.bDumpTextures)
		HiresTexture::Init(SConfig::GetInstance().m_LocalCoreStartupParameter.m_strUniqueID, true);

	SetHash64Function();
	texture_pool_memory_usage = 0;
	invalidate_texture_cache_requested = false;
}

void TextureCache::RequestInvalidateTextureCache()
{
	invalidate_texture_cache_requested = true;
}

void TextureCache::Invalidate()
{
	UnbindTextures();
	for (auto& tex : textures)
	{
		FreeTexture(tex.second);
	}
	textures.clear();
}

void TextureCache::InvalidateHiresCache()
{
	for (auto& tex : hires_texture_pool)
	{
		texture_pool_memory_usage -= tex.second->native_size_in_bytes;
		delete tex.second;
	}
	hires_texture_pool.clear();
}

TextureCache::~TextureCache()
{
	UnbindTextures();
	Invalidate();
	InvalidateHiresCache();
	for (auto& rt : texture_pool)
	{
		delete rt.second;
	}
	texture_pool.clear();
	texture_pool_memory_usage = 0;
	if (TextureCache::temp)
	{
		FreeAlignedMemory(TextureCache::temp);
		TextureCache::temp = nullptr;
	}	
}

void TextureCache::OnConfigChanged(VideoConfig& config)
{
	if (g_texture_cache)
	{
		// TODO: Invalidating texcache is really stupid in some of these cases
		if (config.iSafeTextureCache_ColorSamples != backup_config.s_colorsamples ||
			config.bTexFmtOverlayEnable != backup_config.s_texfmt_overlay ||
			config.bTexFmtOverlayCenter != backup_config.s_texfmt_overlay_center ||
			config.bHiresTextures != backup_config.s_hires_textures ||
			invalidate_texture_cache_requested)
		{
			g_texture_cache->Invalidate();

			TexDecoder_SetTexFmtOverlayOptions(g_ActiveConfig.bTexFmtOverlayEnable, g_ActiveConfig.bTexFmtOverlayCenter);

			if (config.bHiresTextures != backup_config.s_hires_textures)
			{
				if (config.bHiresTextures)
				{
					HiresTexture::Init(SConfig::GetInstance().m_LocalCoreStartupParameter.m_strUniqueID);
				}
				else
				{
					InvalidateHiresCache();
				}
			}
			invalidate_texture_cache_requested = false;
		}
	}

	backup_config.s_colorsamples = config.iSafeTextureCache_ColorSamples;
	backup_config.s_texfmt_overlay = config.bTexFmtOverlayEnable;
	backup_config.s_texfmt_overlay_center = config.bTexFmtOverlayCenter;
	backup_config.s_hires_textures = config.bHiresTextures;
}

void TextureCache::Cleanup(int _frameCount)
{
	int texture_kill_threshold = TEXTURE_KILL_THRESHOLD;
	int pool_kill_threshold = TEXTURE_POOL_KILL_THRESHOLD;
	if (texture_pool_memory_usage < (TEXTURE_POOL_MEMORY_LIMIT / 2))
	{
		// if we are using less than a half of the memory limit don't bother cleaning.
		return;
	}
	else if (texture_pool_memory_usage < TEXTURE_POOL_MEMORY_LIMIT)
	{
		// if we are using less than the memory limit increase kill threshold
		texture_kill_threshold *= 32;
		pool_kill_threshold *= 10;
	}
	TexCache::iterator iter = textures.begin();
	TexCache::iterator tcend = textures.end();
	while (iter != tcend)
	{
		if (iter->second->frameCount == FRAMECOUNT_INVALID)
		{
			iter->second->frameCount = _frameCount;
		}
		if (_frameCount > texture_kill_threshold + iter->second->frameCount &&
			// EFB copies living on the host GPU are unrecoverable and thus shouldn't be deleted
			!iter->second->IsEfbCopy())
		{
			FreeTexture(iter->second);
			iter = textures.erase(iter);
		}
		else
		{
			++iter;
		}
	}
	TexPool::iterator iter2 = texture_pool.begin();
	TexPool::iterator tcend2 = texture_pool.end();
	while (iter2 != tcend2)
	{
		if (iter2->second->frameCount == FRAMECOUNT_INVALID)
		{
			iter2->second->frameCount = _frameCount;
		}
		if (_frameCount > pool_kill_threshold + iter2->second->frameCount)
		{
			texture_pool_memory_usage -= iter2->second->native_size_in_bytes;
			delete iter2->second;
			iter2 = texture_pool.erase(iter2);
		}
		else
		{
			++iter2;
		}
	}
	if (g_ActiveConfig.bHiresTextures && texture_pool_memory_usage > TEXTURE_POOL_MEMORY_LIMIT)
	{
		// We are using to much memory so we will have to clean hires textures if they are not used
		HiresTexPool::iterator iter_hrt = hires_texture_pool.begin();
		HiresTexPool::iterator hrtend = hires_texture_pool.end();
		while (iter_hrt != hrtend)
		{
			if (iter_hrt->second->frameCount == FRAMECOUNT_INVALID)
			{
				iter_hrt->second->frameCount = _frameCount;
			}
			if (_frameCount > HIRES_POOL_KILL_THRESHOLD + iter_hrt->second->frameCount)
			{
				texture_pool_memory_usage -= iter_hrt->second->native_size_in_bytes;
				delete iter_hrt->second;
				iter_hrt = hires_texture_pool.erase(iter_hrt);
			}
			else
			{
				++iter_hrt;
			}
		}
	}
}


void TextureCache::MakeRangeDynamic(u32 start_address, u32 size)
{
	TexCache::iterator
		iter = textures.begin();
	while (iter != textures.end())
	{
		if (iter->second->OverlapsMemoryRange(start_address, size))
		{
			FreeTexture(iter->second);
			iter = textures.erase(iter);
		}
		else
		{
			++iter;
		}
	}
}

bool TextureCache::TCacheEntryBase::OverlapsMemoryRange(u32 range_address, u32 range_size) const
{
	if (addr + size_in_bytes <= range_address)
		return false;

	if (addr >= range_address + range_size)
		return false;

	return true;
}

void TextureCache::DumpTexture(TCacheEntryBase* entry, std::string basename, u32 level)
{
	std::string szDir = File::GetUserPath(D_DUMPTEXTURES_IDX) +
		SConfig::GetInstance().m_LocalCoreStartupParameter.m_strUniqueID;

	// make sure that the directory exists
	if (!File::Exists(szDir) || !File::IsDirectory(szDir))
		File::CreateDir(szDir);

	if (level > 0)
	{
		basename += StringFromFormat("_mip%i", level);
	}
	std::string filename = szDir + "/" + basename + ".png";

	if (!File::Exists(filename))
		entry->Save(filename, level);
}

static u32 CalculateLevelSize(u32 level_0_size, u32 level)
{
	return (level_0_size + ((1 << level) - 1)) >> level;
}

// Used by TextureCache::Load
TextureCache::TCacheEntryBase* TextureCache::ReturnEntry(unsigned int stage, TCacheEntryBase* entry)
{
	entry->frameCount = FRAMECOUNT_INVALID;
	bound_textures[stage] = entry;
	GFX_DEBUGGER_PAUSE_AT(NEXT_TEXTURE_CHANGE, true);
	return entry;
}
void TextureCache::BindTextures()
{
	for (int i = 0; i < 8; ++i)
	{
		if (bound_textures[i])
			bound_textures[i]->Bind(i);
	}
}
void TextureCache::UnbindTextures()
{
	std::fill(std::begin(bound_textures), std::end(bound_textures), nullptr);
}

TextureCache::TCacheEntryBase* TextureCache::Load(const u32 stage)
{
	const FourTexUnits &tex = bpmem.tex[stage >> 2];
	const u32 id = stage & 3;
	const u32 address = (tex.texImage3[id].image_base/* & 0x1FFFFF*/) << 5;
	u32 width = tex.texImage0[id].width + 1;
	u32 height = tex.texImage0[id].height + 1;
	const s32 texformat = tex.texImage0[id].format;
	const u32 tlutaddr = tex.texTlut[id].tmem_offset << 9;
	const u32 tlutfmt = tex.texTlut[id].tlut_format;
	const bool use_mipmaps = (tex.texMode0[id].min_filter & 3) != 0;
	u32 tex_levels = use_mipmaps ? ((tex.texMode1[id].max_lod + 0xf) / 0x10 + 1) : 1;	
	const bool from_tmem = tex.texImage1[id].image_type != 0;

	if (0 == address)
		return nullptr;

	// TexelSizeInNibbles(format) * width * height / 16;
	const u32 bsw = TexDecoder_GetBlockWidthInTexels(texformat) - 1;
	const u32 bsh = TexDecoder_GetBlockHeightInTexels(texformat) - 1;

	u32 expandedWidth = (width + bsw) & (~bsw);
	u32 expandedHeight = (height + bsh) & (~bsh);
	const u32 nativeW = width;
	const u32 nativeH = height;

	// Hash assigned to texcache entry (also used to generate filenames used for texture dumping and custom texture lookup)
	u64 tex_hash = TEXHASH_INVALID;
	u64 tlut_hash = TEXHASH_INVALID;
	u32 full_format = texformat;
	PC_TexFormat pcfmt = PC_TEX_FMT_NONE;

	const bool isPaletteTexture = (texformat == GX_TF_C4 || texformat == GX_TF_C8 || texformat == GX_TF_C14X2);
	
	if (isPaletteTexture)
	{
		// Reject invalid tlut format.
		if (tlutfmt > GX_TL_RGB5A3)
		{
			return nullptr;
		}
		full_format = texformat | (tlutfmt << 16);
	}

	const u32 texture_size = TexDecoder_GetTextureSizeInBytes(expandedWidth, expandedHeight, texformat);

	const u8* src_data;
	if (from_tmem)
		src_data = &texMem[bpmem.tex[stage / 4].texImage1[stage % 4].tmem_even * TMEM_LINE_SIZE];
	else
		src_data = Memory::GetPointer(address);
	
	if (src_data == nullptr)
	{
		ERROR_LOG(VIDEO, "TextureCache::Load has an address in Wii memory (%8x) but not in real memory (NULL)!", address);
		return nullptr;
	}

	// TODO: This doesn't hash GB tiles for preloaded RGBA8 textures (instead, it's hashing more data from the low tmem bank than it should)	
	tex_hash = GetHash64(src_data, texture_size, g_ActiveConfig.iSafeTextureCache_ColorSamples);
	u32 palette_size = 0;
	bool palette_upload_required = false;
	if (isPaletteTexture)
	{
		palette_size = std::min(TexDecoder_GetPaletteSize(texformat), (s32)(TMEM_SIZE - tlutaddr));
		tlut_hash = GetHash64(&texMem[tlutaddr], palette_size, g_ActiveConfig.iSafeTextureCache_ColorSamples);
		palette_upload_required = s_prev_tlut_address != tlutaddr
			|| s_prev_tlut_hash != tlut_hash
			|| s_prev_tlut_size != palette_size;
		s_prev_tlut_address = tlutaddr;
		s_prev_tlut_hash = tlut_hash;
		s_prev_tlut_size = palette_size;
	}

	// GPUs don't like when the specified mipmap count would require more than one 1x1-sized LOD in the mipmap chain
	// e.g. 64x64 with 7 LODs would have the mipmap chain 64x64,32x32,16x16,8x8,4x4,2x2,1x1,0x0, so we limit the mipmap count to 6 there
	tex_levels = std::min<u32>(IntLog2(std::max(width, height)) + 1, tex_levels);
	
	// Find all texture cache entries for the current texture address, and decide whether to use one of
	// them, or to create a new one
	//
	// In most cases, the fastest way is to use only one texture cache entry for the same address. Usually,
	// when a texture changes, the old version of the texture is unlikely to be used again. If there were
	// new cache entries created for normal texture updates, there would be a slowdown due to a huge amount
	// of unused cache entries. Also thanks to texture pooling, overwriting an existing cache entry is
	// faster than creating a new one from scratch.
	//
	// Some games use the same address for different textures though. If the same cache entry was used in
	// this case, it would be constantly overwritten, and effectively there wouldn't be any caching for
	// those textures. Examples for this are Metroid Prime and Castlevania 3. Metroid Prime has multiple
	// sets of fonts on each other stored in a single texture and uses the palette to make different
	// characters visible or invisible. In Castlevania 3 some textures are used for 2 different things or
	// at least in 2 different ways(size 1024x1024 vs 1024x256).
	//
	// To determine whether to use multiple cache entries or a single entry, use the following heuristic:
	// If the same texture address is used several times during the same frame, assume the address is used
	// for different purposes and allow creating an additional cache entry. If there's at least one entry
	// that hasn't been used for the same frame, then overwrite it, in order to keep the cache as small as
	// possible. If the current texture is found in the cache, use that entry.
	//
	// For efb copies, the entry created in CopyRenderTargetToTexture always has to be used, or else it was
	// done in vain.
	std::pair <TexCache::iterator, TexCache::iterator> iter_range = textures.equal_range(address);
	TexCache::iterator iter = iter_range.first;
	TexCache::iterator oldest_entry = iter;
	int temp_frameCount = 0x7fffffff;
	TexCache::iterator unconverted_copy = textures.end();

	while (iter != iter_range.second)
	{
		TCacheEntryBase* entry = iter->second;
		if (entry->IsEfbCopy())
		{
			// EFB copies have slightly different rules: the hash doesn't need to match
			// in EFB2Tex mode, and EFB copy formats have different meanings from texture
			// formats.
			if (g_ActiveConfig.bSkipEFBCopyToRam || (tex_hash == entry->hash))
			{
				// TODO: We should check format/width/height/levels for EFB copies. Checking
				// format is complicated because EFB copy formats don't exactly match
				// texture formats. I'm not sure what effect checking width/height/levels
				// would have.
				if (!isPaletteTexture)
					return ReturnEntry(stage, entry);
				// Note that we found an unconverted EFB copy, then continue. We'll
				// perform the conversion later. Currently, we only convert EFB copies to
				// palette textures; we could do other conversions if it proved to be
				// beneficial.
				unconverted_copy = iter;
			}
			else
			{
				// Aggressively prune EFB copies: if it isn't useful here, it will probably
				// never be useful again. It's theoretically possible for a game to do
				// something weird where the copy could become useful in the future, but in
				// practice it doesn't happen.
				FreeTexture(entry);
				iter = textures.erase(iter);
				continue;
			}
		}
		else
		{
			// For normal textures, all texture parameters need to match
			if (entry->hash == (tex_hash ^ tlut_hash) && entry->format == full_format && entry->native_levels >= tex_levels &&
				entry->native_width == nativeW && entry->native_height == nativeH)
			{
				return ReturnEntry(stage, entry);
			}
		}
		// Find the entry which hasn't been used for the longest time
		if (entry->frameCount != FRAMECOUNT_INVALID && entry->frameCount < temp_frameCount)
		{
			temp_frameCount = entry->frameCount;
			oldest_entry = iter;
		}
		++iter;
	}

	if (unconverted_copy != textures.end())
	{
		// Perform palette decoding.
		TCacheEntryBase *entry = unconverted_copy->second;
		TCacheEntryBase *decoded_entry = AllocateTexture(entry->config);

		decoded_entry->SetGeneralParameters(address, texture_size, full_format);
		decoded_entry->SetDimensions(entry->native_width, entry->native_height, 1);
		decoded_entry->hash = tex_hash ^ tlut_hash;
		decoded_entry->frameCount = FRAMECOUNT_INVALID;
		decoded_entry->is_efb_copy = false;

		if (palette_upload_required)
		{
			g_texture_cache->LoadLut(tlutfmt, &texMem[tlutaddr], palette_size);
		}

		textures.insert(TexCache::value_type(address, decoded_entry));

		// If supported palettize, if not return the original entry
		if (decoded_entry->PalettizeFromBase(entry))
		{
			return ReturnEntry(stage, decoded_entry);
		}
		else
		{
			return ReturnEntry(stage, entry);
		}
	}

	// If at least one entry was not used for the same frame, overwrite the oldest one
	if (temp_frameCount != 0x7fffffff)
	{
		// pool this texture and make a new one later
		FreeTexture(oldest_entry->second);
		textures.erase(oldest_entry);
	}	

	std::unique_ptr<HiresTexture> hires_tex;
	std::string basename;
	if (g_ActiveConfig.bHiresTextures || g_ActiveConfig.bDumpTextures)
	{
		basename = HiresTexture::GenBaseName(
			src_data, texture_size,
			&texMem[tlutaddr], palette_size,
			width, height,
			texformat,
			use_mipmaps, g_ActiveConfig.bDumpTextures);
	}
	if (g_ActiveConfig.bHiresTextures)
	{
		HiresTexPool::iterator hriter = hires_texture_pool.find(basename);
		if (hriter != hires_texture_pool.end())
		{
			textures.insert(TexCache::value_type(address, hriter->second));
			return ReturnEntry(stage, hriter->second);
		}
		hires_tex.reset(HiresTexture::Search(
			basename,
			[](size_t required_size)
			{
				TextureCache::CheckTempSize(required_size);
				return TextureCache::temp;
			}
		));
		if (hires_tex)
		{
			if (hires_tex->m_width != width || hires_tex->m_height != height)
			{
				width = hires_tex->m_width;
				height = hires_tex->m_height;
			}
			expandedWidth = hires_tex->m_width;
			expandedHeight = hires_tex->m_height;
			pcfmt = hires_tex->m_format;
		}
	}
	if (isPaletteTexture && palette_upload_required && !hires_tex)
	{
		g_texture_cache->LoadLut(tlutfmt, &texMem[tlutaddr], palette_size);
	}
	if (pcfmt == PC_TEX_FMT_NONE)
	{
		pcfmt = g_texture_cache->GetNativeTextureFormat(texformat, (TlutFormat)tlutfmt, width, height);
	}
	// how many levels the allocated texture shall have
	const u32 texLevels = hires_tex ? hires_tex->m_levels : tex_levels;

	// create the entry/texture
	TCacheEntryConfig config;
	config.width = width;
	config.height = height;
	config.levels = texLevels;
	config.pcformat = pcfmt;
	TCacheEntryBase* entry = AllocateTexture(config);
	GFX_DEBUGGER_PAUSE_AT(NEXT_NEW_TEXTURE, true);

	textures.insert(TexCache::value_type(address, entry));

	entry->SetGeneralParameters(address, texture_size, full_format);
	entry->SetDimensions(nativeW, nativeH, tex_levels);
	entry->SetHiresParams(!!hires_tex, basename);
	entry->hash = tex_hash ^ tlut_hash;
	entry->is_efb_copy = false;

	// load texture
	if (hires_tex)
	{
		entry->Load(TextureCache::temp, width, height, expandedWidth, 0);
	}
	else
	{
		if (!(texformat == GX_TF_RGBA8 && from_tmem))
		{
			entry->Load(src_data, width, height, expandedWidth,
				expandedHeight, texformat, tlutaddr, (TlutFormat)tlutfmt, 0);
		}
		else
		{
			u8* src_data_gb = &texMem[bpmem.tex[stage / 4].texImage2[stage % 4].tmem_odd * TMEM_LINE_SIZE];
			entry->LoadFromTmem(src_data, src_data_gb, width, height, expandedWidth,
				expandedHeight, 0);
		}
	}

	
	if (g_ActiveConfig.bDumpTextures && !hires_tex)
	{
		DumpTexture(entry, basename, 0);
	}

	if (hires_tex)
	{
		u8 *Bufferptr = TextureCache::temp;
		Bufferptr += TextureUtil::GetTextureSizeInBytes(width, height, pcfmt);
		for (u32 level = 1; level != texLevels; ++level)
		{
			u32 mip_width = CalculateLevelSize(width, level);
			u32 mip_height = CalculateLevelSize(height, level);
			entry->Load(Bufferptr, mip_width, mip_height, mip_width, level);
			Bufferptr += TextureUtil::GetTextureSizeInBytes(mip_width, mip_height, pcfmt);
		}
	}
	else
	{
		src_data += texture_size;

		const u8* ptr_even = NULL;
		const u8* ptr_odd = NULL;
		if (from_tmem)
		{
			ptr_even = &texMem[bpmem.tex[stage / 4].texImage1[stage % 4].tmem_even * TMEM_LINE_SIZE + texture_size];
			ptr_odd = &texMem[bpmem.tex[stage / 4].texImage2[stage % 4].tmem_odd * TMEM_LINE_SIZE];
		}

		for (u32 level = 1; level != texLevels; ++level)
		{
			const u32 mip_width = CalculateLevelSize(width, level);
			const u32 mip_height = CalculateLevelSize(height, level);
			const u32 expanded_mip_width = (mip_width + bsw) & (~bsw);
			const u32 expanded_mip_height = (mip_height + bsh) & (~bsh);

			const u8*& mip_src_data = from_tmem
				? ((level % 2) ? ptr_odd : ptr_even)
				: src_data;
			entry->Load(mip_src_data, mip_width, mip_height, expanded_mip_width,
				expanded_mip_height, texformat, tlutaddr, (TlutFormat)tlutfmt, level);
			mip_src_data += TexDecoder_GetTextureSizeInBytes(expanded_mip_width, expanded_mip_height, texformat);

			if (g_ActiveConfig.bDumpTextures)
				DumpTexture(entry, basename, level);
		}
	}

	INCSTAT(stats.numTexturesCreated);
	SETSTAT(stats.numTexturesAlive, textures.size());

	return ReturnEntry(stage, entry);
}

void TextureCache::CopyRenderTargetToTexture(u32 dstAddr, u32 dstFormat, PEControl::PixelFormat srcFormat,
	const EFBRectangle& srcRect, bool isIntensity, bool scaleByHalf)
{
	// Emulation methods:
	// 
	// - EFB to RAM:
	//		Encodes the requested EFB data at its native resolution to the emulated RAM using shaders.
	//		Load() decodes the data from there again (using TextureDecoder) if the EFB copy is being used as a texture again.
	//		Advantage: CPU can read data from the EFB copy and we don't lose any important updates to the texture
	//		Disadvantage: Encoding+decoding steps often are redundant because only some games read or modify EFB copies before using them as textures.
	// 
	// - EFB to texture:
	//		Copies the requested EFB data to a texture object in VRAM, performing any color conversion using shaders.
	//		Advantage:	Works for many games, since in most cases EFB copies aren't read or modified at all before being used as a texture again.
	//					Since we don't do any further encoding or decoding here, this method is much faster.
	//					It also allows enhancing the visual quality by doing scaled EFB copies.
	// 
	// - Hybrid EFB copies:
	//		1a) Whenever this function gets called, encode the requested EFB data to RAM (like EFB to RAM)
	//		1b) Set type to TCET_EC_DYNAMIC for all texture cache entries in the destination address range.
	//			If EFB copy caching is enabled, further checks will (try to) prevent redundant EFB copies.
	//		2) Check if a texture cache entry for the specified dstAddr already exists (i.e. if an EFB copy was triggered to that address before):
	//		2a) Entry doesn't exist:
	//			- Also copy the requested EFB data to a texture object in VRAM (like EFB to texture)
	//			- Create a texture cache entry for the target (type = TCET_EC_VRAM)
	//			- Store a hash of the encoded RAM data in the texcache entry.
	//		2b) Entry exists AND type is TCET_EC_VRAM:
	//			- Like case 2a, but reuse the old texcache entry instead of creating a new one.
	//		2c) Entry exists AND type is TCET_EC_DYNAMIC:
	//			- Only encode the texture to RAM (like EFB to RAM) and store a hash of the encoded data in the existing texcache entry.
	//			- Do NOT copy the requested EFB data to a VRAM object. Reason: the texture is dynamic, i.e. the CPU is modifying it. Storing a VRAM copy is useless, because we'd always end up deleting it and reloading the data from RAM anyway.
	//		3) If the EFB copy gets used as a texture, compare the source RAM hash with the hash you stored when encoding the EFB data to RAM.
	//		3a) If the two hashes match AND type is TCET_EC_VRAM, reuse the VRAM copy you created
	//		3b) If the two hashes differ AND type is TCET_EC_VRAM, screw your existing VRAM copy. Set type to TCET_EC_DYNAMIC.
	//			Redecode the source RAM data to a VRAM object. The entry basically behaves like a normal texture now.
	//		3c) If type is TCET_EC_DYNAMIC, treat the EFB copy like a normal texture.
	//		Advantage: 	Non-dynamic EFB copies can be visually enhanced like with EFB to texture.
	//					Compatibility is as good as EFB to RAM.
	//		Disadvantage:	Slower than EFB to texture and often even slower than EFB to RAM.
	//						EFB copy cache depends on accurate texture hashing being enabled. However, with accurate hashing you end up being as slow as without a copy cache anyway.
	//
	// Disadvantage of all methods: Calling this function requires the GPU to perform a pipeline flush which stalls any further CPU processing.
	//
	// For historical reasons, Dolphin doesn't actually implement "pure" EFB to RAM emulation, but only EFB to texture and hybrid EFB copies.

	float colmat[28] = { 0 };
	float *const fConstAdd = colmat + 16;
	float *const ColorMask = colmat + 20;
	ColorMask[0] = ColorMask[1] = ColorMask[2] = ColorMask[3] = 255.0f;
	ColorMask[4] = ColorMask[5] = ColorMask[6] = ColorMask[7] = 1.0f / 255.0f;
	u32 cbufid = -1;
	bool efbHasAlpha = bpmem.zcontrol.pixel_format == PEControl::RGBA6_Z24;

	if (srcFormat == PEControl::Z24)
	{
		switch (dstFormat)
		{
		case 0: // Z4
			colmat[3] = colmat[7] = colmat[11] = colmat[15] = 1.0f;
			cbufid = 0;
			break;
		case 1: // Z8
		case 8: // Z8
			colmat[0] = colmat[4] = colmat[8] = colmat[12] = 1.0f;
			cbufid = 1;
			break;

		case 3: // Z16
			colmat[1] = colmat[5] = colmat[9] = colmat[12] = 1.0f;
			cbufid = 2;
			break;

		case 11: // Z16 (reverse order)
			colmat[0] = colmat[4] = colmat[8] = colmat[13] = 1.0f;
			cbufid = 3;
			break;

		case 6: // Z24X8
			colmat[0] = colmat[5] = colmat[10] = 1.0f;
			cbufid = 4;
			break;

		case 9: // Z8M
			colmat[1] = colmat[5] = colmat[9] = colmat[13] = 1.0f;
			cbufid = 5;
			break;

		case 10: // Z8L
			colmat[2] = colmat[6] = colmat[10] = colmat[14] = 1.0f;
			cbufid = 6;
			break;

		case 12: // Z16L - copy lower 16 depth bits
			// expected to be used as an IA8 texture (upper 8 bits stored as intensity, lower 8 bits stored as alpha)
			// Used e.g. in Zelda: Skyward Sword
			colmat[1] = colmat[5] = colmat[9] = colmat[14] = 1.0f;
			cbufid = 7;
			break;

		default:
			ERROR_LOG(VIDEO, "Unknown copy zbuf format: 0x%x", dstFormat);
			colmat[2] = colmat[5] = colmat[8] = 1.0f;
			cbufid = 8;
			break;
		}
	}
	else if (isIntensity)
	{
		fConstAdd[0] = fConstAdd[1] = fConstAdd[2] = 16.0f / 255.0f;
		switch (dstFormat)
		{
		case 0: // I4
		case 1: // I8
		case 2: // IA4
		case 3: // IA8
		case 8: // I8
			// TODO - verify these coefficients
			colmat[0] = 0.257f; colmat[1] = 0.504f; colmat[2] = 0.098f;
			colmat[4] = 0.257f; colmat[5] = 0.504f; colmat[6] = 0.098f;
			colmat[8] = 0.257f; colmat[9] = 0.504f; colmat[10] = 0.098f;

			if (dstFormat < 2 || dstFormat == 8)
			{
				colmat[12] = 0.257f; colmat[13] = 0.504f; colmat[14] = 0.098f;
				fConstAdd[3] = 16.0f / 255.0f;
				if (dstFormat == 0)
				{
					ColorMask[0] = ColorMask[1] = ColorMask[2] = 15.0f;
					ColorMask[4] = ColorMask[5] = ColorMask[6] = 1.0f / 15.0f;
					cbufid = 9;
				}
				else
				{
					cbufid = 10;
				}
			}
			else// alpha
			{
				colmat[15] = 1;
				if (dstFormat == 2)
				{
					ColorMask[0] = ColorMask[1] = ColorMask[2] = ColorMask[3] = 15.0f;
					ColorMask[4] = ColorMask[5] = ColorMask[6] = ColorMask[7] = 1.0f / 15.0f;
					cbufid = 11;
					if (!efbHasAlpha) {
						ColorMask[3] = 0.0f;
						fConstAdd[3] = 1.0f;
						cbufid = 12;
					}
				}
				else
				{
					cbufid = 13;
					if (!efbHasAlpha) {
						ColorMask[3] = 0.0f;
						fConstAdd[3] = 1.0f;
						cbufid = 14;
					}
				}

			}
			break;

		default:
			ERROR_LOG(VIDEO, "Unknown copy intensity format: 0x%x", dstFormat);
			colmat[0] = colmat[5] = colmat[10] = colmat[15] = 1.0f;
			cbufid = 32;
			break;
		}
	}
	else
	{
		switch (dstFormat)
		{
		case 0: // R4
			colmat[0] = colmat[4] = colmat[8] = colmat[12] = 1;
			ColorMask[0] = 15.0f;
			ColorMask[4] = 1.0f / 15.0f;
			cbufid = 15;
			break;
		case 1: // R8
		case 8: // R8
			colmat[0] = colmat[4] = colmat[8] = colmat[12] = 1;
			cbufid = 16;
			break;

		case 2: // RA4
			colmat[0] = colmat[4] = colmat[8] = colmat[15] = 1.0f;
			ColorMask[0] = ColorMask[3] = 15.0f;
			ColorMask[4] = ColorMask[7] = 1.0f / 15.0f;
			cbufid = 17;
			if (!efbHasAlpha) {
				ColorMask[3] = 0.0f;
				fConstAdd[3] = 1.0f;
				cbufid = 18;
			}
			break;
		case 3: // RA8
			colmat[0] = colmat[4] = colmat[8] = colmat[15] = 1.0f;
			cbufid = 19;
			if (!efbHasAlpha) {
				ColorMask[3] = 0.0f;
				fConstAdd[3] = 1.0f;
				cbufid = 20;
			}
			break;

		case 7: // A8
			colmat[3] = colmat[7] = colmat[11] = colmat[15] = 1.0f;
			cbufid = 21;
			if (!efbHasAlpha) {
				ColorMask[3] = 0.0f;
				fConstAdd[0] = 1.0f;
				fConstAdd[1] = 1.0f;
				fConstAdd[2] = 1.0f;
				fConstAdd[3] = 1.0f;
				cbufid = 22;
			}
			break;

		case 9: // G8
			colmat[1] = colmat[5] = colmat[9] = colmat[13] = 1.0f;
			cbufid = 23;
			break;
		case 10: // B8
			colmat[2] = colmat[6] = colmat[10] = colmat[14] = 1.0f;
			cbufid = 24;
			break;

		case 11: // RG8
			colmat[0] = colmat[4] = colmat[8] = colmat[13] = 1.0f;
			cbufid = 25;
			break;

		case 12: // GB8
			colmat[1] = colmat[5] = colmat[9] = colmat[14] = 1.0f;
			cbufid = 26;
			break;

		case 4: // RGB565
			colmat[0] = colmat[5] = colmat[10] = 1.0f;
			ColorMask[0] = ColorMask[2] = 31.0f;
			ColorMask[4] = ColorMask[6] = 1.0f / 31.0f;
			ColorMask[1] = 63.0f;
			ColorMask[5] = 1.0f / 63.0f;
			fConstAdd[3] = 1.0f; // set alpha to 1
			cbufid = 27;
			break;

		case 5: // RGB5A3
			colmat[0] = colmat[5] = colmat[10] = colmat[15] = 1.0f;
			ColorMask[0] = ColorMask[1] = ColorMask[2] = 31.0f;
			ColorMask[4] = ColorMask[5] = ColorMask[6] = 1.0f / 31.0f;
			ColorMask[3] = 7.0f;
			ColorMask[7] = 1.0f / 7.0f;
			cbufid = 28;
			if (!efbHasAlpha) {
				ColorMask[3] = 0.0f;
				fConstAdd[3] = 1.0f;
				cbufid = 29;
			}
			break;
		case 6: // RGBA8
			colmat[0] = colmat[5] = colmat[10] = colmat[15] = 1.0f;
			cbufid = 30;
			if (!efbHasAlpha) {
				ColorMask[3] = 0.0f;
				fConstAdd[3] = 1.0f;
				cbufid = 31;
			}
			break;
		default:
			ERROR_LOG(VIDEO, "Unknown copy color format: 0x%x", dstFormat);
			colmat[0] = colmat[5] = colmat[10] = colmat[15] = 1.0f;
			cbufid = 32;
			break;
		}
	}

	const u32 tex_w = scaleByHalf ? srcRect.GetWidth() / 2 : srcRect.GetWidth();
	const u32 tex_h = scaleByHalf ? srcRect.GetHeight() / 2 : srcRect.GetHeight();

	u32 scaled_tex_w = g_ActiveConfig.bCopyEFBScaled ? Renderer::EFBToScaledX(tex_w) : tex_w;
	u32 scaled_tex_h = g_ActiveConfig.bCopyEFBScaled ? Renderer::EFBToScaledY(tex_h) : tex_h;
	
	// TODO: Implement EFB to Multitexture and EFB to Subtexture

	// remove all texture cache entries at dstAddr
	std::pair <TexCache::iterator, TexCache::iterator> iter_range = textures.equal_range(dstAddr);
	TexCache::iterator iter = iter_range.first;
	while (iter != iter_range.second)
	{
		FreeTexture(iter->second);
		iter = textures.erase(iter);
	}
	
	TCacheEntryConfig config;
	config.rendertarget = true;
	config.width = scaled_tex_w;
	config.height = scaled_tex_h;
	config.layers = 1; // FramebufferManagerBase::GetEFBLayers();
	
	TCacheEntryBase* entry = AllocateTexture(config);


	// TODO: Using the wrong dstFormat, dumb...
	entry->SetGeneralParameters(dstAddr, 0, dstFormat);
	entry->SetDimensions(tex_w, tex_h, 1);
	entry->hash = TEXHASH_INVALID;

	entry->frameCount = FRAMECOUNT_INVALID;
	entry->is_efb_copy = true;
	entry->SetHiresParams(false, "");

	entry->FromRenderTarget(srcFormat, srcRect, isIntensity, scaleByHalf, cbufid, colmat);
	textures.insert(TexCache::value_type(dstAddr, entry));
}

TextureCache::TCacheEntryBase* TextureCache::AllocateTexture(const TCacheEntryConfig& config)
{
	TexPool::iterator iter = texture_pool.find(config);
	if (iter != texture_pool.end())
	{
		TextureCache::TCacheEntryBase* entry = iter->second;
		texture_pool.erase(iter);
		return entry;
	}
	INCSTAT(stats.numTexturesCreated);
	texture_pool_memory_usage += config.GetSizeInBytes();
	return g_texture_cache->CreateTexture(config);
}

void TextureCache::FreeTexture(TCacheEntryBase* entry)
{
	entry->frameCount = FRAMECOUNT_INVALID;
	if (entry->is_custom_tex)
	{
		hires_texture_pool[entry->basename] = entry;
	}
	else
	{
		texture_pool.insert(TexPool::value_type(entry->config, entry));
	}
	
}