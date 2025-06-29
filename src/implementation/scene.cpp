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
#include <pragma/game/game_resources.hpp>
#include <pragma/model/model.h>
#include <pragma/model/modelmesh.h>
#include <pragma/entities/baseentity.h>
#include <pragma/entities/entity_component_system_t.hpp>
#include <pragma/entities/components/c_animated_component.hpp>
#include <pragma/entities/components/c_eye_component.hpp>
#include <pragma/entities/components/c_vertex_animated_component.hpp>
#include <pragma/entities/components/c_light_map_component.hpp>
#include <pragma/entities/components/c_render_component.hpp>
#include <pragma/entities/components/c_model_component.hpp>
#include <pragma/entities/components/lightmap_data_cache.hpp>
#include <pragma/entities/c_skybox.h>
#include <pragma/rendering/shaders/c_shader_cubemap_to_equirectangular.hpp>
#include <pragma/rendering/shaders/particles/c_shader_particle.hpp>
#include <sharedutils/util_file.h>
#include <util_texture_info.hpp>
#include <texturemanager/texture.h>
#include <cmaterialmanager.h>
#include <datasystem_color.h>
#include <datasystem_vector.h>

// ccl happens to have the same include guard name as sharedutils, so we have to undef it here
#undef __UTIL_STRING_H__
#include <sharedutils/util_string.h>

module pragma.modules.scenekit;

import pragma.scenekit;
import :scene;
import :shader;

using namespace pragma::modules;

scenekit::Cache::ShaderInfo::ShaderInfo() {}

scenekit::Scene::Scene(pragma::scenekit::Scene &rtScene) : m_rtScene {rtScene.shared_from_this()}
{
	m_cache = std::make_shared<Cache>(rtScene.GetRenderMode());
	m_cache->GetModelCache().SetUnique(true);
}

void scenekit::Scene::AddRoughnessMapImageTextureNode(pragma::scenekit::ShaderModuleRoughness &shader, Material &mat, float defaultRoughness) const
{
#if 0
	// If no roughness map is available, just use roughness or specular factor directly
	auto roughnessFactor = defaultRoughness;
	auto hasRoughnessFactor = mat.GetDataBlock()->GetFloat("roughness_factor",&roughnessFactor);

	float specularFactor;
	if(mat.GetDataBlock()->GetFloat("specular_factor",&specularFactor))
	{
		if(hasRoughnessFactor == false)
			roughnessFactor = 1.f;
		roughnessFactor *= (1.f -specularFactor);
		hasRoughnessFactor = true;
	}
	shader.SetRoughnessFactor(roughnessFactor);

	auto rmaPath = prepare_texture(***m_rtScene,mat.GetRMAMap(),PreparedTextureInputFlags::None,nullptr,"white");
	if(rmaPath.has_value() == false)
	{
#if 0
		// No roughness map available; Attempt to use specular map instead
		auto specularPath = prepare_texture(m_scene,mat.GetSpecularMap(),PreparedTextureInputFlags::None);
		if(specularPath.has_value())
		{
			shader.SetSpecularMap(*specularPath);
			if(hasRoughnessFactor == false)
				shader.SetRoughnessFactor(1.f);
		}
#endif
	}
	else
	{
		shader.SetRoughnessMap(*rmaPath);
		if(hasRoughnessFactor == false)
			shader.SetRoughnessFactor(1.f);
	}
#endif
}

void scenekit::Scene::SetAOBakeTarget(BaseEntity &ent, uint32_t matIndex)
{
	std::shared_ptr<pragma::scenekit::Object> oAo;
	std::shared_ptr<pragma::scenekit::Object> oEnv;
	m_cache->AddAOBakeTarget(ent, matIndex, oAo, oEnv);
	m_rtScene->SetBakeTarget(*oAo);
}

void scenekit::Scene::SetAOBakeTarget(Model &mdl, uint32_t matIndex)
{
	std::shared_ptr<pragma::scenekit::Object> oAo;
	std::shared_ptr<pragma::scenekit::Object> oEnv;
	m_cache->AddAOBakeTarget(mdl, matIndex, oAo, oEnv);
	m_rtScene->SetBakeTarget(*oAo);
}

scenekit::Cache &scenekit::Scene::GetCache() { return *m_cache; }

void scenekit::Scene::Finalize()
{
	if(m_finalized)
		return;
	m_finalized = true;
	BuildLightMapObject();
	m_rtScene->AddModelsFromCache(m_cache->GetModelCache());
}

pragma::scenekit::Object *scenekit::Scene::FindObject(const std::string &name)
{
	auto &cache = GetCache();
	auto &mdlCache = cache.GetModelCache();
	for(auto &chunk : mdlCache.GetChunks()) {
		auto &objs = chunk.GetObjects();
		auto it = std::find_if(objs.begin(), objs.end(), [&name](const pragma::scenekit::PObject &obj) { return obj->GetName() == name; });
		if(it == objs.end())
			continue;
		return it->get();
	}
	return nullptr;
}

scenekit::Renderer::Renderer(Scene &scene, pragma::scenekit::Renderer &renderer) : m_scene {scene.shared_from_this()}, m_renderer {renderer.shared_from_this()} {}

void scenekit::Renderer::ReloadShaders()
{
	// Can only reload shaders that are part of this scene's parimary cache
	auto &shaderTranslationTable = m_scene->GetCache().GetRTShaderToShaderTable();
	for(auto &mdlCache : (*m_scene)->GetModelCaches()) {
		for(auto &chunk : mdlCache->GetChunks()) {
			for(auto &mesh : chunk.GetMeshes()) {
				auto renderMesh = m_renderer->FindRenderMeshByHash(mesh->GetHash());
				if(renderMesh == nullptr)
					continue;
				auto &meshShaders = renderMesh->GetSubMeshShaders();
				for(auto i = decltype(meshShaders.size()) {0u}; i < meshShaders.size(); ++i) {
					auto &rtShader = meshShaders.at(i);
					auto it = shaderTranslationTable.find(rtShader.get());
					if(it == shaderTranslationTable.end())
						continue;
					auto &shader = it->second;
					/*auto newRtShader = pragma::scenekit::Shader::Create<pragma::scenekit::GenericShader>();
					newRtShader->combinedPass = ;
					newRtShader->albedoPass = rtShader->albedoPass;
					newRtShader->normalPass = rtShader->normalPass;
					newRtShader->depthPass = rtShader->depthPass;
					shaderTranslationTable.erase(it);
					shaderTranslationTable[newRtShader.get()] = shader;

					rtShader = newRtShader;*/

					// TODO: Cache
					// TODO
					/*auto pass = shader->InitializeCombinedPass();
					auto cclShader = pragma::scenekit::CCLShader::Create(*m_renderer,*pass);
					cclShader->Finalize(*m_scene);
					renderMesh->GetCyclesMesh()->used_shaders.at(i) = **cclShader;
					renderMesh->GetCyclesMesh()->tag_update(**m_scene,false);*/
				}
			}
		}
	}
}

void scenekit::Scene::BuildLightMapObject()
{
	if(m_lightMapTargets.empty())
		return;
	std::vector<ModelSubMesh *> targetMeshes {};
	std::vector<util::Uuid> targetMeshEntityUuids;
	std::vector<std::shared_ptr<pragma::modules::scenekit::Cache::MeshData>> meshDatas;
	for(auto &hEnt : m_lightMapTargets) {
		if(hEnt.IsValid() == false || hEnt->GetModel() == nullptr)
			continue;
		auto &t = hEnt->GetPose();
		std::vector<ModelSubMesh *> entMeshes;
		auto meshes = m_cache->AddEntityMesh(*hEnt.get(), &entMeshes, nullptr, nullptr, "", t);
		if(meshes.empty() == false) {
			targetMeshes.reserve(targetMeshes.size() + entMeshes.size());
			targetMeshEntityUuids.reserve(targetMeshEntityUuids.size() + entMeshes.size());
			for(auto *mesh : entMeshes) {
				targetMeshes.push_back(mesh);
				targetMeshEntityUuids.push_back(hEnt->GetUuid());
			}
			meshDatas.reserve(meshDatas.size() + meshes.size());
			for(auto &mesh : meshes)
				meshDatas.push_back(mesh);
		}
	}

	if(meshDatas.empty())
		return;

	auto mesh = m_cache->BuildMesh("bake_target", meshDatas);
	if(mesh == nullptr)
		return;

	auto o = pragma::scenekit::Object::Create(*mesh);
	if(o == nullptr)
		return;
	m_cache->GetModelCache().GetChunks().front().AddObject(*o);

	// Lightmap uvs per mesh
	auto numTris = mesh->GetTriangleCount();
	std::vector<Vector2> cclLightmapUvs {};
	cclLightmapUvs.resize(numTris * 3);
	size_t uvOffset = 0;
	for(uint32_t idx = 0; auto *subMesh : targetMeshes) {
		auto &verts = subMesh->GetVertices();
		const std::vector<Vector2> *uvSet = nullptr;
		if(m_lightMapDataCache)
			uvSet = m_lightMapDataCache->FindLightmapUvs(targetMeshEntityUuids.at(idx), subMesh->GetUuid());
		else
			uvSet = subMesh->GetUVSet("lightmap");
		if(uvSet) {
			if(uvSet->size() != verts.size()) {
				Con::cwar << "WARNING: Number of UV coordinates (" << uvSet->size() << ") in lightmap UV set does not match number of mesh vertices (" << verts.size() << ") of mesh with uuid " << util::uuid_to_string(subMesh->GetUuid()) << " of entity with uuid "
				          << util::uuid_to_string(targetMeshEntityUuids.at(idx)) << "! Mesh will be ignored!" << std::endl;
				++idx;
				continue;
			}
			if(uvOffset + verts.size() > cclLightmapUvs.size())
				throw std::logic_error {"Number of mesh vertices exceeds expected number!"};
			for(auto i = decltype(verts.size()) {0u}; i < verts.size(); ++i)
				cclLightmapUvs.at(uvOffset + i) = uvSet->at(i);
		}
		uvOffset += verts.size();
		++idx;
	}
	mesh->SetLightmapUVs(std::move(cclLightmapUvs));
	m_rtScene->SetBakeTarget(*o);
}
void scenekit::Scene::AddLightmapBakeTarget(BaseEntity &ent) { m_lightMapTargets.push_back(ent.GetHandle()); }
void scenekit::Scene::SetLightmapDataCache(LightmapDataCache *cache) { m_lightMapDataCache = cache ? cache->shared_from_this() : nullptr; }

pragma::scenekit::PShader scenekit::Cache::CreateShader(Material &mat, const std::string &meshName, const ShaderInfo &shaderInfo) const
{
	auto it = m_materialToShader.find(&mat);
	if(it != m_materialToShader.end())
		return m_shaderCache->GetShader(it->second);
	auto &matShader = mat.GetShaderIdentifier();
	if(ustring::compare<std::string>(matShader, "nodraw", false))
		return nullptr;
	std::string cyclesShader = "pbr";
	auto &shaderManager = get_shader_manager();
	if(mat.HasPropertyBlock("cycles"))
		cyclesShader = mat.GetProperty<std::string>("cycles/shader", "pbr");
	else {
		auto matShader = mat.GetShaderIdentifier();
		ustring::to_lower(matShader);
		if(shaderManager.IsShaderRegistered(matShader))
			cyclesShader = matShader;
	}
	if(ustring::compare<std::string>(cyclesShader, "nodraw", false))
		return nullptr;

	auto shader = shaderManager.CreateShader(get_node_manager(), cyclesShader, shaderInfo.entity.has_value() ? *shaderInfo.entity : nullptr, shaderInfo.subMesh.has_value() ? *shaderInfo.subMesh : nullptr, mat);
	if(shader == nullptr)
		return nullptr;
	auto rtShader = pragma::scenekit::Shader::Create<pragma::scenekit::GenericShader>();
	m_rtShaderToShader[rtShader.get()] = shader;

	auto &hairConfig = shader->GetHairConfig();
	if(hairConfig.has_value())
		rtShader->SetHairConfig(*hairConfig);

	auto &subdivSettings = shader->GetSubdivisionSettings();
	if(subdivSettings.has_value())
		rtShader->SetSubdivisionSettings(*subdivSettings);

	rtShader->combinedPass = shader->InitializeCombinedPass();
	rtShader->albedoPass = shader->InitializeAlbedoPass();
	rtShader->normalPass = shader->InitializeNormalPass();
	rtShader->depthPass = shader->InitializeDepthPass();
	auto idx = m_shaderCache->AddShader(*rtShader);
	m_materialToShader[&mat] = idx;
	return rtShader;

#if 0
	// Make sure all textures have finished loading
	static_cast<CMaterialManager&>(get_client_state()->GetMaterialManager()).GetTextureManager().WaitForTextures();

	TextureInfo *diffuseMap = nullptr;
	if(ustring::compare(mat.GetShaderIdentifier(),"skybox",false))
		diffuseMap = mat.GetTextureInfo("skybox");
	else
		diffuseMap = mat.GetDiffuseMap();
	PreparedTextureOutputFlags flags;
	auto diffuseTexPath = prepare_texture(***m_rtScene,diffuseMap,PreparedTextureInputFlags::CanBeEnvMap,&flags);
	if(diffuseTexPath.has_value() == false)
		return nullptr;

	std::optional<std::string> albedo2TexPath = {};
	if(ustring::compare(mat.GetShaderIdentifier(),"pbr_blend",false))
	{
		auto *albedo2Map = mat.GetTextureInfo(Material::ALBEDO_MAP2_IDENTIFIER);
		if(albedo2Map)
			albedo2TexPath = prepare_texture(***m_rtScene,albedo2Map,PreparedTextureInputFlags::None);
	}

	// TODO: Only allow toon shader when baking diffuse lighting
	const std::string bsdfName = "bsdf_scene";

	if(umath::is_flag_set(flags,PreparedTextureOutputFlags::Envmap))
		return nullptr;

	/*if(true)
	{
	// TODO: Hair shader requires meshes with curves
	auto nodeBsdf = shader->AddNode("principled_hair_bsdf",bsdfName);
	auto *pNodeBsdf = static_cast<ccl::PrincipledHairBsdfNode*>(**nodeBsdf);

	// Default settings (Taken from Blender)
	pNodeBsdf->roughness = 0.3f;
	pNodeBsdf->radial_roughness = 0.3f;
	pNodeBsdf->coat = 100.f;
	pNodeBsdf->ior = 1.55f;
	pNodeBsdf->offset = 2.f;
	pNodeBsdf->random_roughness = 0.f;

	auto hasAlbedoNode = umath::is_flag_set(flags,PreparedTextureOutputFlags::Envmap) ?
	(AssignEnvironmentTexture(*shader,"albedo",*diffuseTexPath) != nullptr) :
	(AssignTexture(*shader,"albedo",*diffuseTexPath) != nullptr);
	if(hasAlbedoNode)
	shader->Link("albedo","color","output","surface");

	// Roughness map
	//LinkRoughnessMap(*shader,*mat,bsdfName);

	// Normal map
	//LinkNormalMap(*shader,*mat,meshName,bsdfName,"normal");
	return shader;
	}
	*/

	auto fApplyColorFactor = [&mat](pragma::scenekit::ShaderAlbedoSet &albedoSet) {
		auto &colorFactor = mat.GetDataBlock()->GetValue("color_factor");
		if(colorFactor == nullptr || typeid(*colorFactor) != typeid(ds::Vector4))
			return;
		auto &color = static_cast<ds::Vector4*>(colorFactor.get())->GetValue();
		albedoSet.SetColorFactor(color);
	};

	pragma::scenekit::PShader resShader = nullptr;
	auto renderMode = m_rtScene->GetRenderMode();
	if(renderMode == pragma::scenekit::Scene::RenderMode::SceneAlbedo)
	{
		auto shader = pragma::scenekit::Shader::Create<pragma::scenekit::ShaderAlbedo>(*m_rtScene,meshName +"_shader");
		shader->SetMeshName(meshName);
		shader->GetAlbedoSet().SetAlbedoMap(*diffuseTexPath);
		if(albedo2TexPath.has_value())
		{
			shader->GetAlbedoSet2().SetAlbedoMap(*albedo2TexPath);
			shader->SetUseVertexAlphasForBlending(true);
		}
		shader->SetAlphaMode(mat.GetAlphaMode(),mat.GetAlphaCutoff());
		resShader = shader;
	}
	else if(renderMode == pragma::scenekit::Scene::RenderMode::SceneNormals)
	{
		auto shader = pragma::scenekit::Shader::Create<pragma::scenekit::ShaderNormal>(*m_rtScene,meshName +"_shader");
		shader->SetMeshName(meshName);
		shader->GetAlbedoSet().SetAlbedoMap(*diffuseTexPath);
		if(albedo2TexPath.has_value())
		{
			shader->GetAlbedoSet2().SetAlbedoMap(*albedo2TexPath);
			shader->SetUseVertexAlphasForBlending(true);
		}
		shader->SetAlphaMode(mat.GetAlphaMode(),mat.GetAlphaCutoff());
		auto normalTexPath = prepare_texture(***m_rtScene,mat.GetNormalMap(),PreparedTextureInputFlags::None);
		if(normalTexPath.has_value())
			shader->SetNormalMap(*normalTexPath);
		resShader = shader;
	}
	else if(renderMode == pragma::scenekit::Scene::RenderMode::SceneDepth)
	{
		auto shader = pragma::scenekit::Shader::Create<pragma::scenekit::ShaderDepth>(*m_rtScene,meshName +"_shader");
		shader->SetMeshName(meshName);
		static_cast<pragma::scenekit::ShaderDepth&>(*shader).SetFarZ(m_rtScene->GetCamera().GetFarZ());
		shader->GetAlbedoSet().SetAlbedoMap(*diffuseTexPath);
		if(albedo2TexPath.has_value())
		{
			shader->GetAlbedoSet2().SetAlbedoMap(*albedo2TexPath);
			shader->SetUseVertexAlphasForBlending(true);
		}
		shader->SetAlphaMode(mat.GetAlphaMode(),mat.GetAlphaCutoff());
		resShader = shader;
	}
	else
	{
		std::string cyclesShader = "pbr";
		auto &dataBlock = mat.GetDataBlock();
		auto cyclesBlock = dataBlock->GetBlock("cycles");
		std::shared_ptr<ds::Block> cyclesShaderPropBlock = nullptr;
		if(cyclesBlock)
		{
			cyclesShader = cyclesBlock->GetString("shader","pbr");
			cyclesShaderPropBlock = cyclesBlock->GetBlock("shader_properties");
		}
		std::optional<float> ior {};
		float f;
		if(dataBlock->GetFloat("ior",&f))
			ior = f;

		if(ustring::compare(cyclesShader,"toon",false))
		{
			auto shader = pragma::scenekit::Shader::Create<pragma::scenekit::ShaderToon>(*m_rtScene,meshName +"_shader");
			fApplyColorFactor(shader->GetAlbedoSet());
			shader->SetMeshName(meshName);
			shader->GetAlbedoSet().SetAlbedoMap(*diffuseTexPath);
			if(albedo2TexPath.has_value())
			{
				shader->GetAlbedoSet2().SetAlbedoMap(*albedo2TexPath);
				shader->SetUseVertexAlphasForBlending(true);
			}
			
			auto normalTexPath = prepare_texture(***m_rtScene,mat.GetNormalMap(),PreparedTextureInputFlags::None);
			if(normalTexPath.has_value())
				shader->SetNormalMap(*normalTexPath);

			if(cyclesShaderPropBlock)
			{
				auto *shaderToon = static_cast<pragma::scenekit::ShaderToon*>(shader.get());
				float value;
				if(cyclesShaderPropBlock->GetFloat("diffuse_size",&value))
					shaderToon->SetDiffuseSize(value);
				if(cyclesShaderPropBlock->GetFloat("diffuse_smooth",&value))
					shaderToon->SetDiffuseSmooth(value);
				if(cyclesShaderPropBlock->GetFloat("specular_size",&value))
					shaderToon->SetSpecularSize(value);
				if(cyclesShaderPropBlock->GetFloat("specular_smooth",&value))
					shaderToon->SetSpecularSmooth(value);

				auto &dvSpecFactor = cyclesShaderPropBlock->GetValue("specular_color");
				if(dvSpecFactor != nullptr && typeid(*dvSpecFactor) == typeid(ds::Color))
				{
					auto &color = static_cast<ds::Color&>(*dvSpecFactor).GetValue();
					shader->SetSpecularColor(color.ToVector3());
				}

				auto &dvShadeColor = cyclesShaderPropBlock->GetValue("shade_color");
				if(dvShadeColor != nullptr && typeid(*dvShadeColor) == typeid(ds::Color))
				{
					auto &color = static_cast<ds::Color&>(*dvShadeColor).GetValue();
					shader->SetShadeColor(color.ToVector3());
				}
			}

			resShader = shader;
		}
		else if(ustring::compare(cyclesShader,"glass",false))
		{
			auto shader = pragma::scenekit::Shader::Create<pragma::scenekit::ShaderGlass>(*m_rtScene,meshName +"_shader");
			fApplyColorFactor(shader->GetAlbedoSet());
			shader->SetMeshName(meshName);
			shader->GetAlbedoSet().SetAlbedoMap(*diffuseTexPath);
			if(albedo2TexPath.has_value())
			{
				shader->GetAlbedoSet2().SetAlbedoMap(*albedo2TexPath);
				shader->SetUseVertexAlphasForBlending(true);
			}
			auto normalTexPath = prepare_texture(***m_rtScene,mat.GetNormalMap(),PreparedTextureInputFlags::None);
			if(normalTexPath.has_value())
				shader->SetNormalMap(*normalTexPath);

			// Roughness map
			AddRoughnessMapImageTextureNode(*shader,mat,0.5f);

			if(ior.has_value())
				shader->SetIOR(*ior);

			resShader = shader;
		}
		else
		{
			std::shared_ptr<pragma::scenekit::ShaderPBR> shader = nullptr;
			auto isParticleSystemShader = (shaderInfo.particleSystem.has_value() && shaderInfo.particle.has_value());
			if(isParticleSystemShader)
			{
				shader = pragma::scenekit::Shader::Create<pragma::scenekit::ShaderParticle>(*m_rtScene,meshName +"_shader");
				auto *ptShader = static_cast<pragma::scenekit::ShaderParticle*>(shader.get());
				auto *pt = static_cast<const pragma::CParticleSystemComponent::ParticleData*>(*shaderInfo.particle);
				Color color {static_cast<int16_t>(pt->color.at(0)),static_cast<int16_t>(pt->color.at(1)),static_cast<int16_t>(pt->color.at(2)),static_cast<int16_t>(pt->color.at(3))};
				ptShader->SetColor(color);

				auto *pShader = static_cast<pragma::ShaderParticle*>(pragma::get_cengine()->GetShader("particle").get());
				if(pShader)
				{
					auto renderFlags = ShaderParticle::RenderFlags::None;
					auto ptcFlags = pShader->GetRenderFlags(**shaderInfo.particleSystem,pragma::ParticleRenderFlags::None);
					if((*shaderInfo.particleSystem)->GetEffectiveAlphaMode() == pragma::ParticleAlphaMode::AdditiveByColor)
						renderFlags |= ShaderParticle::RenderFlags::AdditiveBlendByColor;
				}
			}
			else
				shader = pragma::scenekit::Shader::Create<pragma::scenekit::ShaderPBR>(*m_rtScene,meshName +"_shader");
			fApplyColorFactor(shader->GetAlbedoSet());
			shader->SetMeshName(meshName);

			auto data = mat.GetDataBlock();
			auto dataSSS = data->GetBlock("subsurface_scattering");
			if(dataSSS)
			{
				float subsurface;
				if(dataSSS->GetFloat("factor",&subsurface))
					shader->SetSubsurface(subsurface);

				auto &dvSubsurfaceColor = dataSSS->GetValue("color_factor");
				if(dvSubsurfaceColor != nullptr && typeid(*dvSubsurfaceColor) == typeid(ds::Vector))
				{
					auto &color = static_cast<ds::Vector&>(*dvSubsurfaceColor).GetValue();
					shader->SetSubsurfaceColorFactor(color);
				}

				const std::unordered_map<std::string,pragma::scenekit::PrincipledBSDFNode::SubsurfaceMethod> bssrdfToCclType = {
					{"cubic",pragma::scenekit::PrincipledBSDFNode::SubsurfaceMethod::Cubic},
					{"gaussian",pragma::scenekit::PrincipledBSDFNode::SubsurfaceMethod::Gaussian},
					{"principled",pragma::scenekit::PrincipledBSDFNode::SubsurfaceMethod::Principled},
					{"burley",pragma::scenekit::PrincipledBSDFNode::SubsurfaceMethod::Burley},
					{"random_walk",pragma::scenekit::PrincipledBSDFNode::SubsurfaceMethod::RandomWalk},
					{"principled_random_walk",pragma::scenekit::PrincipledBSDFNode::SubsurfaceMethod::PrincipledRandomWalk}
				};

				std::string subsurfaceMethod;
				if(dataSSS->GetString("method",&subsurfaceMethod))
				{
					auto it = bssrdfToCclType.find(subsurfaceMethod);
					if(it != bssrdfToCclType.end())
						shader->SetSubsurfaceMethod(it->second);
				}

				auto &dvSubsurfaceRadius = dataSSS->GetValue("scatter_color");
				if(dvSubsurfaceRadius != nullptr && typeid(*dvSubsurfaceRadius) == typeid(ds::Color))
					shader->SetSubsurfaceRadius(static_cast<ds::Color&>(*dvSubsurfaceRadius).GetValue().ToVector3());
			}
			//

			// Note: We always need the albedo texture information for the translucency.
			// Whether metalness/roughness/etc. affect baking in any way is unclear (probably not),
			// but it also doesn't hurt to have them.

			if(ior.has_value())
				shader->SetIOR(*ior);

			// Albedo map
			shader->GetAlbedoSet().SetAlbedoMap(*diffuseTexPath);
			if(albedo2TexPath.has_value())
			{
				shader->GetAlbedoSet2().SetAlbedoMap(*albedo2TexPath);
				shader->SetUseVertexAlphasForBlending(true);
			}
			if(mat.GetAlphaMode() != AlphaMode::Opaque && data->GetBool("black_to_alpha"))
				shader->SetFlags(pragma::scenekit::Shader::Flags::AdditiveByColor,true);

			// Normal map
			auto normalTexPath = prepare_texture(***m_rtScene,mat.GetNormalMap(),PreparedTextureInputFlags::None);
			if(normalTexPath)
				shader->SetNormalMap(*normalTexPath);

			// Metalness map
			auto metalnessTexPath = prepare_texture(***m_rtScene,mat.GetRMAMap(),PreparedTextureInputFlags::None,nullptr,"white");
			if(metalnessTexPath)
				shader->SetMetalnessMap(*metalnessTexPath);

			// If no metalness map is available, just use metalness factor directly
			float metalnessFactor;
			if(mat.GetDataBlock()->GetFloat("metalness_factor",&metalnessFactor))
				shader->SetMetalnessFactor(metalnessFactor);

			// Roughness map
			AddRoughnessMapImageTextureNode(*shader,mat,0.5f);

			// Emission map
			auto globalEmissionStrength = m_rtScene->GetEmissionStrength();
			shader->SetEmissionIntensity(globalEmissionStrength);
			if(globalEmissionStrength > 0.f)
			{
				auto *emissionTex = mat.GetGlowMap();
				static auto particleLightEmissionFactor = 0.f;
				if(particleLightEmissionFactor > 0.f && isParticleSystemShader)
				{
					emissionTex = emissionTex ? emissionTex : diffuseMap;
					auto strength = particleLightEmissionFactor;
					shader->SetEmissionFactor({strength,strength,strength});
				}
				auto emissionTexPath = prepare_texture(***m_rtScene,emissionTex,PreparedTextureInputFlags::None);
				if(emissionTexPath)
				{
					shader->SetEmissionMap(*emissionTexPath);
					if(data->GetBool("glow_alpha_only") == true)
					{
						shader->SetEmissionFromAlbedoAlpha(*shader,true);

						// Glow intensity
						auto glowBlendDiffuseScale = 1.f;
						data->GetFloat("glow_blend_diffuse_scale",&glowBlendDiffuseScale);

						auto glowScale = 1.f;
						data->GetFloat("glow_scale",&glowScale);

						auto glowIntensity = glowBlendDiffuseScale *glowScale +1.f; // +1 to match Pragma more closely
						auto strength = glowIntensity;
						shader->SetEmissionFactor({strength,strength,strength});
					}
				}

				auto valEmissionFactor = mat.GetDataBlock()->GetValue("emission_factor");
				if(valEmissionFactor && typeid(*valEmissionFactor) == typeid(ds::Vector))
				{
					auto &emissionFactor = static_cast<ds::Vector&>(*valEmissionFactor).GetValue();
					shader->SetEmissionFactor(emissionFactor);
				}
			}

			// Wrinkle maps
			auto wrinkleStretchMap = prepare_texture(***m_rtScene,mat.GetTextureInfo(Material::WRINKLE_STRETCH_MAP_IDENTIFIER),PreparedTextureInputFlags::None);
			if(wrinkleStretchMap)
				shader->SetWrinkleStretchMap(*wrinkleStretchMap);

			auto wrinkleCompressMap = prepare_texture(***m_rtScene,mat.GetTextureInfo(Material::WRINKLE_COMPRESS_MAP_IDENTIFIER),PreparedTextureInputFlags::None);
			if(wrinkleCompressMap)
				shader->SetWrinkleCompressMap(*wrinkleCompressMap);
			resShader = shader;
		}
		if(resShader)
		{
			resShader->SetAlphaMode(mat.GetAlphaMode(),mat.GetAlphaCutoff());
			auto &dataBlock = mat.GetDataBlock();
			float alphaFactor;
			if(dataBlock->GetFloat("alpha_factor",&alphaFactor))
				resShader->SetAlphaFactor(alphaFactor);
		}
	}
	if(resShader && shaderInfo.entity.has_value() && shaderInfo.subMesh.has_value())
	{
		auto normalMapSpace = pragma::scenekit::NormalMapNode::Space::Tangent;
		if(ustring::compare(mat.GetShaderIdentifier(),"eye",false))
		{
			//normalMapSpace = NormalMapNode::Space::Object;

			// Eye shader; We'll have to do some additional steps to get the proper UV coordinates.
			auto eyeC = (*shaderInfo.entity)->GetComponent<CEyeComponent>();
			auto &mdl = *(*shaderInfo.entity)->GetModel();
			uint32_t eyeballIndex;
			if(eyeC.valid() && eyeC->FindEyeballIndex((*shaderInfo.subMesh)->GetSkinTextureIndex(),eyeballIndex))
			{
				auto *eyeballData = eyeC->GetEyeballData(eyeballIndex);
				auto *eyeball = mdl.GetEyeball(eyeballIndex);
				if(eyeball && eyeballData)
				{
					Vector4 irisProjU,irisProjV;
					if(eyeC->GetEyeballProjectionVectors(eyeballIndex,irisProjU,irisProjV))
					{
						auto dilationFactor = eyeballData->config.dilation;
						auto maxDilationFactor = eyeball->maxDilationFactor;
						auto irisUvRadius = eyeball->irisUvRadius;
						auto uvHandler = std::make_shared<pragma::scenekit::UVHandlerEye>(irisProjU,irisProjV,dilationFactor,maxDilationFactor,irisUvRadius);
						resShader->SetUVHandler(pragma::scenekit::Shader::TextureType::Albedo,uvHandler);
						resShader->SetUVHandler(pragma::scenekit::Shader::TextureType::Emission,uvHandler);
					}
				}
			}
		}

		auto *normalModule = dynamic_cast<pragma::scenekit::ShaderModuleNormal*>(resShader.get());
		if(normalModule)
			normalModule->SetNormalMapSpace(normalMapSpace);
	}
	return resShader;
#endif
	return nullptr;
}
