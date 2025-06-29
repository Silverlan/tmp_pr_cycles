/* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/.
*
* Copyright (c) 2023 Silverlan
*/

module;

#include <pragma/c_engine.h>
#include <prosper_context.hpp>
#include <buffers/prosper_uniform_resizable_buffer.hpp>
#include <buffers/prosper_dynamic_resizable_buffer.hpp>
#include <pragma/clientstate/clientstate.h>
#include <texturemanager/texture.h>
#include <cmaterialmanager.h>
#include <cmaterial_manager2.hpp>
#include <sharedutils/util_file.h>
#include <pragma/rendering/shaders/c_shader_cubemap_to_equirectangular.hpp>
#include <pragma/rendering/shaders/particles/c_shader_particle.hpp>
#include <util_texture_info.hpp>
#include <util_image.hpp>
#include <fsys/ifile.hpp>

module pragma.modules.scenekit;

import :texture;

static std::optional<std::string> get_abs_error_texture_path()
{
	std::string errTexPath = "materials\\error.dds";
	std::string absPath;
	if(FileManager::FindAbsolutePath(errTexPath, absPath))
		return absPath;
	return {};
}

enum class PreparedTextureInputFlags : uint8_t { None = 0u, CanBeEnvMap = 1u };
REGISTER_BASIC_BITWISE_OPERATORS(PreparedTextureInputFlags)
enum class PreparedTextureOutputFlags : uint8_t { None = 0u, Envmap = 1u };
REGISTER_BASIC_BITWISE_OPERATORS(PreparedTextureOutputFlags)

static std::optional<std::string> prepare_texture(std::shared_ptr<Texture> &tex, bool &outSuccess, bool &outConverted, PreparedTextureInputFlags inFlags, PreparedTextureOutputFlags *optOutFlags, const std::optional<std::string> &defaultTexture, bool translucent)
{
	if(optOutFlags)
		*optOutFlags = PreparedTextureOutputFlags::None;

	outSuccess = false;
	outConverted = false;
	std::string texName {};
	// Make sure texture has been fully loaded!
	if(tex == nullptr || tex->IsLoaded() == false) {
		tex = nullptr;
		if(defaultTexture.has_value()) {
			auto &texManager = static_cast<msys::CMaterialManager &>(pragma::get_client_state()->GetMaterialManager()).GetTextureManager();
			auto ptrTex = texManager.LoadAsset(*defaultTexture);
			if(ptrTex != nullptr) {
				texName = *defaultTexture;
				tex = ptrTex;
				if(tex->IsLoaded() == false)
					tex = nullptr;
			}
		}
	}
	else
		texName = tex->GetName();
	if(tex == nullptr || tex->IsError() || tex->HasValidVkTexture() == false)
		return get_abs_error_texture_path();

	/*if(tex->IsLoaded() == false)
	{
	TextureManager::LoadInfo loadInfo {};
	loadInfo.flags = TextureLoadFlags::LoadInstantly;
	static_cast<CMaterialManager&>(pragma::get_client_state()->GetMaterialManager()).GetTextureManager().Load(*pragma::get_cengine(),texInfo->name,loadInfo);
	if(tex->IsLoaded() == false)
	return get_abs_error_texture_path();
	}
	*/
	ufile::remove_extension_from_filename(texName); // DDS-writer will add the extension for us

	auto vkTex = tex->GetVkTexture();
	auto *img = &vkTex->GetImage();
	auto isCubemap = img->IsCubemap();
	if(isCubemap) {
		if(umath::is_flag_set(inFlags, PreparedTextureInputFlags::CanBeEnvMap) == false)
			return {};
		// Image is a cubemap, which Cycles doesn't support! We'll have to convert it to a equirectangular image and use that instead.
		auto &shader = static_cast<pragma::ShaderCubemapToEquirectangular &>(*pragma::get_cengine()->GetShader("cubemap_to_equirectangular"));
		auto equiRectMap = shader.CubemapToEquirectangularTexture(*vkTex);
		vkTex = equiRectMap;
		img = &vkTex->GetImage();
		texName += "_equirect";

		if(optOutFlags)
			*optOutFlags |= PreparedTextureOutputFlags::Envmap;
	}

	enum class Format { DDS = 0, PNG };
	auto format = Format::DDS;
	if(translucent) {
		// Transparent DDS textures sometimes cause weird emission artifacts (with transparent areas
		// appearing emissive in bright white), so we'll use png for those textures instead.
		format = Format::PNG;
	}

	auto texPath = "materials\\" + texName;
	std::string ext = (format == Format::DDS) ? "dds" : "png";

	std::string absPath;
	texPath += "." + ext;
	// Check if a version of the texture already exists, in which case we can just use it directly!
	if(FileManager::FindAbsolutePath(texPath, absPath)) {
		outSuccess = true;
		return absPath;
	}

	// Texture does not have the right format to begin with or does not exist on the local hard drive.
	// We will have to create the texture file in the right format (if the texture object is valid).
	if(tex == nullptr)
		return get_abs_error_texture_path(); // Texture is not valid! Return error texture.

	std::string baseOutputPath = "addons/converted/";
	if(format == Format::PNG) {
		auto imgBuf = img->ToHostImageBuffer(uimg::Format::RGBA8, prosper::ImageLayout::ShaderReadOnlyOptimal);
		auto fullPath = baseOutputPath + texPath;
		FileManager::CreatePath(ufile::get_path_from_filename(fullPath).c_str());
		auto f = filemanager::open_file(fullPath, filemanager::FileMode::Write | filemanager::FileMode::Binary);
		fsys::File fp {f};
		uimg::save_image(fp, *imgBuf, uimg::ImageFormat::PNG);
		if(FileManager::FindAbsolutePath(texPath, absPath)) {
			outSuccess = true;
			return absPath;
		}
		return get_abs_error_texture_path();
	}

	// Output path for the DDS-file we're about to create
	auto ddsPath = baseOutputPath + "materials/" + texName;
	uimg::TextureInfo imgWriteInfo {};
	imgWriteInfo.containerFormat = uimg::TextureInfo::ContainerFormat::DDS; // Cycles doesn't support KTX
	if(tex->HasFlag(Texture::Flags::SRGB))
		imgWriteInfo.flags |= uimg::TextureInfo::Flags::SRGB;

	// Try to determine appropriate formats
	if(tex->HasFlag(Texture::Flags::NormalMap)) {
		imgWriteInfo.inputFormat = uimg::TextureInfo::InputFormat::R32G32B32A32_Float;
		imgWriteInfo.SetNormalMap();
	}
	else {
		auto format = img->GetFormat();
		if(prosper::util::is_16bit_format(format)) {
			imgWriteInfo.inputFormat = uimg::TextureInfo::InputFormat::R16G16B16A16_Float;
			imgWriteInfo.outputFormat = uimg::TextureInfo::OutputFormat::HDRColorMap;
		}
		else if(prosper::util::is_32bit_format(format) || prosper::util::is_64bit_format(format)) {
			imgWriteInfo.inputFormat = uimg::TextureInfo::InputFormat::R32G32B32A32_Float;
			imgWriteInfo.outputFormat = uimg::TextureInfo::OutputFormat::HDRColorMap;
		}
		else {
			imgWriteInfo.inputFormat = uimg::TextureInfo::InputFormat::R8G8B8A8_UInt;
			// TODO: Check the alpha channel values to determine whether we actually need a full alpha channel?
			imgWriteInfo.outputFormat = uimg::TextureInfo::OutputFormat::ColorMapSmoothAlpha;
		}
		switch(format) {
		case prosper::Format::BC1_RGBA_SRGB_Block:
		case prosper::Format::BC1_RGBA_UNorm_Block:
		case prosper::Format::BC1_RGB_SRGB_Block:
		case prosper::Format::BC1_RGB_UNorm_Block:
			imgWriteInfo.outputFormat = uimg::TextureInfo::OutputFormat::BC1;
			break;
		case prosper::Format::BC2_SRGB_Block:
		case prosper::Format::BC2_UNorm_Block:
			imgWriteInfo.outputFormat = uimg::TextureInfo::OutputFormat::BC2;
			break;
		case prosper::Format::BC3_SRGB_Block:
		case prosper::Format::BC3_UNorm_Block:
			imgWriteInfo.outputFormat = uimg::TextureInfo::OutputFormat::BC3;
			break;
		case prosper::Format::BC4_SNorm_Block:
		case prosper::Format::BC4_UNorm_Block:
			imgWriteInfo.outputFormat = uimg::TextureInfo::OutputFormat::BC4;
			break;
		case prosper::Format::BC5_SNorm_Block:
		case prosper::Format::BC5_UNorm_Block:
			imgWriteInfo.outputFormat = uimg::TextureInfo::OutputFormat::BC5;
			break;
		case prosper::Format::BC6H_SFloat_Block:
		case prosper::Format::BC6H_UFloat_Block:
			// TODO: As of 20-03-26, Cycles (/oiio) does not have support for BC6, so we'll
			// fall back to a different format
			imgWriteInfo.inputFormat = uimg::TextureInfo::InputFormat::R16G16B16A16_Float;
			// imgWriteInfo.outputFormat = uimg::TextureInfo::OutputFormat::BC6;
			imgWriteInfo.outputFormat = uimg::TextureInfo::OutputFormat::DXT5;
			break;
		case prosper::Format::BC7_SRGB_Block:
		case prosper::Format::BC7_UNorm_Block:
			// TODO: As of 20-03-26, Cycles (/oiio) does not have support for BC7, so we'll
			// fall back to a different format
			imgWriteInfo.inputFormat = uimg::TextureInfo::InputFormat::R16G16B16A16_Float;
			// imgWriteInfo.outputFormat = uimg::TextureInfo::OutputFormat::BC7;
			imgWriteInfo.outputFormat = uimg::TextureInfo::OutputFormat::DXT1;
			break;
		}
	}
	absPath = "";
	// Save the DDS image and make sure the file exists
	if(pragma::get_client_game()->SaveImage(*img, ddsPath, imgWriteInfo) && FileManager::FindAbsolutePath(ddsPath + ".dds", absPath)) {
		outSuccess = true;
		outConverted = true;
		return absPath;
	}
	// Something went wrong, fall back to error texture!
	return get_abs_error_texture_path();
}

static std::optional<std::string> prepare_texture(std::shared_ptr<Texture> &texInfo, PreparedTextureInputFlags inFlags, PreparedTextureOutputFlags *optOutFlags, const std::optional<std::string> &defaultTexture, bool translucent)
{
	if(optOutFlags)
		*optOutFlags = PreparedTextureOutputFlags::None;
	if(texInfo == nullptr)
		return {};
	auto success = false;
	auto converted = false;
	auto result = prepare_texture(texInfo, success, converted, inFlags, optOutFlags, defaultTexture, translucent);
	if(success == false && texInfo->GetName() != "error") {
		Con::cwar << "WARNING: Unable to prepare texture '";
		if(texInfo)
			Con::cwar << texInfo->GetName();
		else
			Con::cwar << "Unknown";
		Con::cwar << "'! Using error texture instead..." << Con::endl;
	}
	else {
		if(converted)
			Con::cout << "Converted texture '" << texInfo->GetName() << "' to DDS!" << Con::endl;

#if 0
		// TODO: Re-implement this
		ccl::ImageMetaData metaData;
		if(scene.image_manager->get_image_metadata(*result,nullptr,ccl::u_colorspace_raw,metaData) == false)
		{
			Con::cwar<<"WARNING: Texture '"<<texInfo->name<<"' has format which is incompatible with cycles! Falling back to error texture..."<<Con::endl;
			result = get_abs_error_texture_path();
			if(scene.image_manager->get_image_metadata(*result,nullptr,ccl::u_colorspace_raw,metaData) == false)
			{
				Con::cwar<<"WARNING: Error texture also not compatible! Falling back to untextured!"<<Con::endl;
				result = {};
			}
		}
#endif
	}

	return result;
}

std::optional<std::string> pragma::modules::scenekit::prepare_texture(const std::string &texPath, const std::optional<std::string> &defaultTexture, bool translucent)
{
	auto &texManager = static_cast<msys::CMaterialManager &>(pragma::get_client_state()->GetMaterialManager()).GetTextureManager();
	auto ptex = texManager.LoadAsset(texPath);
	auto flags = PreparedTextureInputFlags::CanBeEnvMap;
	PreparedTextureOutputFlags retFlags;
	auto tex = ptex;
	return ::prepare_texture(tex, flags, &retFlags, defaultTexture, translucent);
}
