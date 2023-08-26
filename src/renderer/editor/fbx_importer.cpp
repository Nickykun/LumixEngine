#include "fbx_importer.h"
#include "animation/animation.h"
#include "editor/asset_compiler.h"
#include "editor/studio_app.h"
#include "editor/world_editor.h"
#include "engine/atomic.h"
#include "engine/crt.h"
#include "engine/engine.h"
#include "engine/file_system.h"
#include "engine/hash.h"
#include "engine/job_system.h"
#include "engine/log.h"
#include "engine/math.h"
#include "engine/os.h"
#include "engine/path.h"
#include "engine/prefab.h"
#include "engine/profiler.h"
#include "engine/resource_manager.h"
#include "engine/world.h"
#include "meshoptimizer/meshoptimizer.h"
#include "mikktspace/mikktspace.h"
#include "physics/physics_resources.h"
#include "physics/physics_system.h"
#include "renderer/draw_stream.h"
#include "renderer/material.h"
#include "renderer/model.h"
#include "renderer/pipeline.h"
#include "renderer/render_module.h"
#include "renderer/renderer.h"
#include "renderer/shader.h"
#include "renderer/voxels.h"


namespace Lumix {


static bool hasTangents(const ofbx::Mesh& geom) {
	if (geom.getTangents()) return true;
	if (geom.getUVs()) return true;
	return false;
}


static void getMaterialName(const ofbx::Material* material, char (&out)[128])
{
	copyString(out, material ? material->name : "default");
	char* iter = out;
	while (*iter)
	{
		char c = *iter;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
		{
			*iter = '_';
		}
		++iter;
	}
	makeLowercase(Span(out), out);
}


void FBXImporter::getImportMeshName(const ImportMesh& mesh, char (&out)[256])
{
	const char* name = mesh.fbx->name;
	const ofbx::Material* material = mesh.fbx_mat;

	if (name[0] == '\0' && mesh.fbx->getParent()) name = mesh.fbx->getParent()->name;
	if (name[0] == '\0' && material) name = material->name;
	copyString(out, name);
	if(mesh.submesh >= 0) {
		catString(out, "_");
		char tmp[32];
		toCString(mesh.submesh, Span(tmp));
		catString(out, tmp);
	}
}


const FBXImporter::ImportMesh* FBXImporter::getAnyMeshFromBone(const ofbx::Object* node, int bone_idx) const
{
	for (int i = 0; i < m_meshes.size(); ++i)
	{
		const ofbx::Mesh* mesh = m_meshes[i].fbx;
		if (m_meshes[i].bone_idx == bone_idx) {
			return &m_meshes[i];
		}

		auto* skin = mesh->getSkin();
		if (!skin) continue;

		for (int j = 0, c = skin->getClusterCount(); j < c; ++j)
		{
			if (skin->getCluster(j)->getLink() == node) return &m_meshes[i];
		}
	}
	return nullptr;
}


static ofbx::Matrix makeOFBXIdentity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }


static ofbx::Matrix getBindPoseMatrix(const FBXImporter::ImportMesh* mesh, const ofbx::Object* node)
{
	if (!mesh) return node->getGlobalTransform();
	if (!mesh->fbx) return makeOFBXIdentity();

	auto* skin = mesh->fbx->getSkin();
	if (!skin) return node->getGlobalTransform();

	for (int i = 0, c = skin->getClusterCount(); i < c; ++i)
	{
		const ofbx::Cluster* cluster = skin->getCluster(i);
		if (cluster->getLink() == node)
		{
			return cluster->getTransformLinkMatrix();
		}
	}
	return node->getGlobalTransform();
}

static StringView toStringView(ofbx::DataView data) {
	return StringView(
		(const char*)data.begin,
		(const char*)data.end
	);
}

static void extractEmbedded(const ofbx::IScene& m_scene, StringView src_dir)
{
	PROFILE_FUNCTION();
	for (int i = 0, c = m_scene.getEmbeddedDataCount(); i < c; ++i) {
		const ofbx::DataView embedded = m_scene.getEmbeddedData(i);

		StringView filename = toStringView(m_scene.getEmbeddedFilename(i));
		const PathInfo pi(filename);
		const StaticString<MAX_PATH> fullpath(src_dir, pi.basename, ".", pi.extension);

		if (os::fileExists(fullpath)) return;

		os::OutputFile file;
		if (!file.open(fullpath)) {
			logError("Failed to save ", fullpath);
			return;
		}

		if (!file.write(embedded.begin + 4, embedded.end - embedded.begin - 4)) {
			logError("Failed to write ", fullpath);
		}
		file.close();
	}
}


bool FBXImporter::findTexture(StringView src_dir, StringView ext, FBXImporter::ImportTexture& tex) const {
	PathInfo file_info(tex.path);
	tex.src = src_dir;
	tex.src.append(file_info.basename, ".", ext);
	tex.is_valid = m_filesystem.fileExists(tex.src);

	if (!tex.is_valid) {
		tex.src = src_dir;
		tex.src.append(file_info.dir, "/", file_info.basename, ".", ext);
		tex.is_valid = m_filesystem.fileExists(tex.src);
					
		if (!tex.is_valid) {
			tex.src = src_dir;
			tex.src.append("textures/", file_info.basename, ".", ext);
			tex.is_valid = m_filesystem.fileExists(tex.src);
		}
	}
	return tex.is_valid;
}

void FBXImporter::gatherMaterials(StringView fbx_filename, StringView src_dir)
{
	PROFILE_FUNCTION();
	for (ImportMesh& mesh : m_meshes)
	{
		const ofbx::Material* fbx_mat = mesh.fbx_mat;
		if (!fbx_mat) continue;

		ImportMaterial& mat = m_materials.emplace();
		mat.fbx = fbx_mat;
	}

	Array<String> names(m_allocator);
	for (ImportMaterial& mat : m_materials) {
		char name[128];
		getMaterialName(mat.fbx, name);
		if (m_material_name_map.find(mat.fbx).isValid()) continue;

		u32 collision = 0;
		if (names.find([&](const String& i){ return i == name; }) != -1) {
			char orig_name[128];
			copyString(orig_name, name);
			do {
				copyString(name, orig_name);
				char num[16];
				toCString(collision, Span(num));
				catString(name, num);
				++collision;
			} while(names.find([&](const String& i){ return i == name; }) != -1);
		}
		names.emplace(name, m_allocator);
		m_material_name_map.insert(mat.fbx, names.last());
	}

	for (ImportMaterial& material : m_materials) {
		if (!material.import) continue;

		const String& mat_name = m_material_name_map[material.fbx];

		const Path mat_src(src_dir, mat_name, ".mat");
		if (m_filesystem.fileExists(mat_src)) material.import = false;
	}

	for (ImportMaterial& mat : m_materials) {
		if (!mat.import) continue;

		auto gatherTexture = [this, &mat, src_dir, fbx_filename](ofbx::Texture::TextureType type) {
			const ofbx::Texture* texture = mat.fbx->getTexture(type);
			if (!texture) return;

			ImportTexture& tex = mat.textures[type];
			tex.fbx = texture;
			ofbx::DataView filename = tex.fbx->getRelativeFileName();
			if (filename == "") filename = tex.fbx->getFileName();
			tex.path = toStringView(filename);
			tex.src = tex.path;
			tex.is_valid = m_filesystem.fileExists(tex.src);

			StringView tex_ext = Path::getExtension(tex.path);
			if (!tex.is_valid && !findTexture(src_dir, tex_ext, tex)) {
				const char* exts[] = { "dds", "png", "jpg", "jpeg", "tga", "bmp" };
				for (const char* ext : exts) {
					if (findTexture(src_dir, ext, tex)) break;
				}
			}

			Path::normalize(tex.src.data);

			if (!tex.is_valid) {
				logInfo(fbx_filename, ": texture ", tex.src, " not found");
				tex.src = "";
			}

			tex.import = true;
		};

		gatherTexture(ofbx::Texture::DIFFUSE);
		gatherTexture(ofbx::Texture::NORMAL);
		gatherTexture(ofbx::Texture::SPECULAR);
	}
}


void FBXImporter::insertHierarchy(Array<const ofbx::Object*>& bones, const ofbx::Object* node)
{
	if (!node) return;
	if (bones.indexOf(node) >= 0) return;
	ofbx::Object* parent = node->getParent();
	insertHierarchy(bones, parent);
	bones.push(node);
}


void FBXImporter::sortBones(bool force_skinned) {
	const int count = m_bones.size();
	u32 first_nonroot = 0;
	for (i32 i = 0; i < count; ++i) {
		if (!m_bones[i]->getParent() ) {
			swap(m_bones[i], m_bones[first_nonroot]);
			++first_nonroot;
		}
	}

	for (i32 i = 0; i < count; ++i)
	{
		for (int j = i + 1; j < count; ++j)
		{
			if (m_bones[i]->getParent() == m_bones[j])
			{
				const ofbx::Object* bone = m_bones[j];
				m_bones.swapAndPop(j);
				m_bones.insert(i, bone);
				--i;
				break;
			}
		}
	}

	if (force_skinned) {
		for (ImportMesh& m : m_meshes) {
			m.bone_idx = m_bones.indexOf(m.fbx);
			m.is_skinned = true;
		}
	}
}


void FBXImporter::gatherBones(const ofbx::IScene& m_scene, bool force_skinned)
{
	PROFILE_FUNCTION();
	for (const ImportMesh& mesh : m_meshes) {
		const ofbx::Skin* skin = mesh.fbx->getSkin();
		if (skin) {
			for (int i = 0; i < skin->getClusterCount(); ++i) {
				const ofbx::Cluster* cluster = skin->getCluster(i);
				insertHierarchy(m_bones, cluster->getLink());
			}
		}

		if (force_skinned) {
			insertHierarchy(m_bones, mesh.fbx);
		}
	}

	for (int i = 0, n = m_scene.getAnimationStackCount(); i < n; ++i) {
		const ofbx::AnimationStack* stack = m_scene.getAnimationStack(i);
		for (int j = 0; stack->getLayer(j); ++j) {
			const ofbx::AnimationLayer* layer = stack->getLayer(j);
			for (int k = 0; layer->getCurveNode(k); ++k) {
				const ofbx::AnimationCurveNode* node = layer->getCurveNode(k);
				if (node->getBone()) insertHierarchy(m_bones, node->getBone());
			}
		}
	}

	m_bones.removeDuplicates();
	sortBones(force_skinned);
}


void FBXImporter::gatherAnimations(const ofbx::IScene& m_scene)
{
	PROFILE_FUNCTION();
	int anim_count = m_scene.getAnimationStackCount();
	for (int i = 0; i < anim_count; ++i) {
		ImportAnimation& anim = m_animations.emplace();
		anim.scene = &m_scene;
		anim.fbx = (const ofbx::AnimationStack*)m_scene.getAnimationStack(i);
		anim.import = true;
		const ofbx::TakeInfo* take_info = m_scene.getTakeInfo(anim.fbx->name);
		if (take_info) {
			if (take_info->name.begin != take_info->name.end) {
				anim.name = toStringView(take_info->name);
			}
			if (anim.name.empty() && take_info->filename.begin != take_info->filename.end) {
				StringView tmp = toStringView(take_info->filename);
				anim.name = Path::getBasename(tmp);
			}
			if (anim.name.empty()) anim.name = "anim";
		}
		else {
			anim.name = "";
		}

		const ofbx::AnimationLayer* anim_layer = anim.fbx->getLayer(0);
		if (!anim_layer || !anim_layer->getCurveNode(0)) {
			m_animations.pop();
		}

		bool data_found = false;
		for (int k = 0; anim_layer->getCurveNode(k); ++k) {
			const ofbx::AnimationCurveNode* node = anim_layer->getCurveNode(k);
			if (node->getBoneLinkProperty() == "Lcl Translation"
				|| node->getBoneLinkProperty() == "Lcl Rotation")
			{
				data_found = true;
				break;
			}
		}
		if (!data_found) m_animations.pop();
	}

	if (m_animations.size() == 1) {
		m_animations[0].name = "";
	}
}


static Vec3 toLumixVec3(const ofbx::Vec3& v) { return {(float)v.x, (float)v.y, (float)v.z}; }


static Matrix toLumix(const ofbx::Matrix& mtx)
{
	Matrix res;

	for (int i = 0; i < 16; ++i) (&res.columns[0].x)[i] = (float)mtx.m[i];

	return res;
}


static u32 packF4u(const Vec3& vec)
{
	const i8 xx = i8(clamp((vec.x * 0.5f + 0.5f) * 255, 0.f, 255.f) - 128);
	const i8 yy = i8(clamp((vec.y * 0.5f + 0.5f) * 255, 0.f, 255.f) - 128);
	const i8 zz = i8(clamp((vec.z * 0.5f + 0.5f) * 255, 0.f, 255.f) - 128);
	const i8 ww = i8(0);

	union {
		u32 ui32;
		i8 arr[4];
	} un;

	un.arr[0] = xx;
	un.arr[1] = yy;
	un.arr[2] = zz;
	un.arr[3] = ww;

	return un.ui32;
}


void FBXImporter::writePackedVec3(const ofbx::Vec3& vec, const Matrix& mtx, OutputMemoryStream* blob) const
{
	Vec3 v = toLumixVec3(vec);
	v = normalize((mtx * Vec4(v, 0)).xyz());
	v = fixOrientation(v);

	u32 packed = packF4u(v);
	blob->write(packed);
}


static void writeUV(const ofbx::Vec2& uv, OutputMemoryStream* blob)
{
	Vec2 tex_cooords = {(float)uv.x, 1 - (float)uv.y};
	blob->write(tex_cooords);
}


static void writeColor(const ofbx::Vec4& color, OutputMemoryStream* blob)
{
	u8 rgba[4];
	rgba[0] = u8(color.x * 255);
	rgba[1] = u8(color.y * 255);
	rgba[2] = u8(color.z * 255);
	rgba[3] = u8(color.w * 255);
	blob->write(rgba);
}


static void writeSkin(const FBXImporter::Skin& skin, OutputMemoryStream* blob)
{
	blob->write(skin.joints);
	blob->write(skin.weights);
	float sum = skin.weights[0] + skin.weights[1] + skin.weights[2] + skin.weights[3];
	ASSERT(sum > 0.99f && sum < 1.01f);
}


static int getMaterialIndex(const ofbx::Mesh& mesh, const ofbx::Material& material)
{
	for (int i = 0, c = mesh.getMaterialCount(); i < c; ++i)
	{
		if (mesh.getMaterial(i) == &material) return i;
	}
	return -1;
}


static void centerMesh(const ofbx::Vec3* vertices, int vertices_count, bool bottom, Matrix& transform, Vec3& center)
{
	if (vertices_count <= 0) return;

	ofbx::Vec3 min = vertices[0];
	ofbx::Vec3 max = vertices[0];

	for (int i = 1; i < vertices_count; ++i)
	{
		ofbx::Vec3 v = vertices[i];
			
		min.x = minimum(min.x, v.x);
		min.y = minimum(min.y, v.y);
		min.z = minimum(min.z, v.z);
			
		max.x = maximum(max.x, v.x);
		max.y = maximum(max.y, v.y);
		max.z = maximum(max.z, v.z);
	}

	center.x = float(min.x + max.x) * 0.5f;
	center.y = float(min.y + max.y) * 0.5f;
	center.z = float(min.z + max.z) * 0.5f;
		
	if (bottom) center.y = (float)min.y;

	const Vec3 p = transform.getTranslation();
	transform.setTranslation(-center);
	center += p;
}


static ofbx::Vec3 operator-(const ofbx::Vec3& a, const ofbx::Vec3& b)
{
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}


static ofbx::Vec2 operator-(const ofbx::Vec2& a, const ofbx::Vec2& b)
{
	return {a.x - b.x, a.y - b.y};
}


static void computeTangentsSimple(Array<ofbx::Vec3>& out, i32 vertex_count, const ofbx::Vec3* vertices, const ofbx::Vec2* uvs) {
	out.resize(vertex_count);
	memset(out.begin(), 0, out.byte_size());
	for (int i = 0; i < vertex_count; i += 3) {
		const ofbx::Vec3 v0 = vertices[i + 0];
		const ofbx::Vec3 v1 = vertices[i + 1];
		const ofbx::Vec3 v2 = vertices[i + 2];
		const ofbx::Vec2 uv0 = uvs[i + 0];
		const ofbx::Vec2 uv1 = uvs[i + 1];
		const ofbx::Vec2 uv2 = uvs[i + 2];

		const ofbx::Vec3 dv10 = v1 - v0;
		const ofbx::Vec3 dv20 = v2 - v0;
		const ofbx::Vec2 duv10 = uv1 - uv0;
		const ofbx::Vec2 duv20 = uv2 - uv0;

		const float dir = duv20.x * duv10.y - duv20.y * duv10.x < 0 ? -1.f : 1.f;
		ofbx::Vec3 tangent; 
		tangent.x = (dv20.x * duv10.y - dv10.x * duv20.y) * dir;
		tangent.y = (dv20.y * duv10.y - dv10.y * duv20.y) * dir;
		tangent.z = (dv20.z * duv10.y - dv10.z * duv20.y) * dir;
		const float l = 1 / sqrtf(float(tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z));
		tangent.x *= l;
		tangent.y *= l;
		tangent.z *= l;
		out[i + 0] = tangent;
		out[i + 1] = tangent;
		out[i + 2] = tangent;
	}
}

static void computeNormals(Array<ofbx::Vec3>& out, const ofbx::Vec3* vertices, i32 vertex_count, const u32* remap, IAllocator& allocator) {
	out.resize(vertex_count);
	Array<u32> count(allocator);
	Array<Vec3> normals(allocator);
	normals.resize(vertex_count);
	count.resize(vertex_count);
	memset(normals.begin(), 0, normals.byte_size());
	memset(count.begin(), 0, count.byte_size());

	auto toLumix = [](const ofbx::Vec3& a) { return Vec3{(float)a.x, (float)a.y, (float)a.z}; };
	auto fromLumix = [](const Vec3& a) { return ofbx::Vec3{ a.x, a.y, a.z}; };
	u32 m = 0;

	for (int i = 0; i < vertex_count; i+= 3) {
		const Vec3 v0 = toLumix(vertices[i + 0]);
		const Vec3 v1 = toLumix(vertices[i + 1]);
		const Vec3 v2 = toLumix(vertices[i + 2]);
		const Vec3 n = normalize(cross(v1 - v0, v2 - v0));
		++count[remap[i + 0]];
		++count[remap[i + 1]];
		++count[remap[i + 2]];
		normals[remap[i + 0]] = normals[remap[i + 0]] + n;
		normals[remap[i + 1]] = normals[remap[i + 1]] + n;
		normals[remap[i + 2]] = normals[remap[i + 2]] + n;

		m = maximum(m ,remap[i]);
		m = maximum(m, remap[i + 1]);
		m = maximum(m, remap[i + 2]);
	}

	for (u32 i = 0; i < m; ++i) {
		if (count[i] > 0) normals[i] = normals[i] / (float)count[i];
	}

	for (i32 i = 0; i < vertex_count; ++i) {
		out[i] = fromLumix(normals[remap[i]]);
	}
}

static void computeTangents(Array<ofbx::Vec3>& out, i32 vertex_count, const ofbx::Vec3* vertices, const ofbx::Vec3* normals, const ofbx::Vec2* uvs, const Path& path)
{
	out.resize(vertex_count);

	struct {
		Array<ofbx::Vec3>* out;
		i32 vertex_count;
		const ofbx::Vec3* vertices;
		const ofbx::Vec3* normals;
		const ofbx::Vec2* uvs;
	} data;

	data.out = &out;
	data.vertex_count = vertex_count;
	data.vertices = vertices;
	data.normals = normals;
	data.uvs = uvs;

	SMikkTSpaceInterface iface = {};
	iface.m_getNumFaces = [](const SMikkTSpaceContext * pContext) -> int {
		auto* ptr = (decltype(data)*)pContext->m_pUserData;
		return ptr->vertex_count / 3;
	};
	iface.m_getNumVerticesOfFace = [](const SMikkTSpaceContext * pContext, const int face) -> int { return 3; };
	iface.m_getPosition = [](const SMikkTSpaceContext * pContext, float fvPosOut[], const int iFace, const int iVert) { 
		auto* ptr = (decltype(data)*)pContext->m_pUserData;
		ofbx::Vec3 p = ptr->vertices[iFace * 3 + iVert];
		fvPosOut[0] = (float)p.x;
		fvPosOut[1] = (float)p.y;
		fvPosOut[2] = (float)p.z;
	};
	iface.m_getNormal = [](const SMikkTSpaceContext * pContext, float fvNormOut[], const int iFace, const int iVert) { 
		auto* ptr = (decltype(data)*)pContext->m_pUserData;
		ofbx::Vec3 p = ptr->normals[iFace * 3 + iVert];
		fvNormOut[0] = (float)p.x;
		fvNormOut[1] = (float)p.y;
		fvNormOut[2] = (float)p.z;
	};
	iface.m_getTexCoord = [](const SMikkTSpaceContext * pContext, float fvTexcOut[], const int iFace, const int iVert) { 
		auto* ptr = (decltype(data)*)pContext->m_pUserData;
		ofbx::Vec2 p = ptr->uvs[iFace * 3 + iVert];
		fvTexcOut[0] = (float)p.x;
		fvTexcOut[1] = (float)p.y;
	};
	iface.m_setTSpaceBasic  = [](const SMikkTSpaceContext * pContext, const float fvTangent[], const float fSign, const int iFace, const int iVert) {
		auto* ptr = (decltype(data)*)pContext->m_pUserData;
		ofbx::Vec3 t;
		t.x = fvTangent[0];
		t.y = fvTangent[1];
		t.z = fvTangent[2];
		(*ptr->out)[iFace * 3 + iVert] = t;
	};

	SMikkTSpaceContext ctx;
	ctx.m_pUserData = &data;
	ctx.m_pInterface = &iface;
	tbool res = genTangSpaceDefault(&ctx);
	if (!res) {
		logError(path, ": failed to generate tangent space");
	}
}

static bool doesFlipHandness(const Matrix& mtx) {
	Vec3 x(1, 0, 0);
	Vec3 y(0, 1, 0);
	Vec3 z = mtx.inverted().transformVector(cross(mtx.transformVector(x), mtx.transformVector(y)));
	return z.z < 0;
}

void FBXImporter::postprocessMeshes(const ImportConfig& cfg, const Path& path)
{
	jobs::forEach(m_meshes.size(), 1, [&](i32 mesh_idx, i32){
		PROFILE_BLOCK("FBXImporter postprocessMeshes")
		ImportMesh& import_mesh = m_meshes[mesh_idx];
		const ofbx::Mesh* mesh = import_mesh.fbx;
		const int vertex_count = mesh->getVertexCount();
		const ofbx::Vec3* vertices = mesh->getVertices();
		const ofbx::Vec3* normals = mesh->getNormals();
		const ofbx::Vec3* tangents = mesh->getTangents();
		const ofbx::Vec4* colors = cfg.import_vertex_colors ? mesh->getColors() : nullptr;
		const ofbx::Vec2* uvs = mesh->getUVs();

		import_mesh.geom_indices.resize(vertex_count);
		meshopt_Stream streams[8];
		u32 stream_count = 0;
		streams[stream_count++] = {vertices, sizeof(vertices[0]), sizeof(vertices[0])};
		if (normals) streams[stream_count++] = {normals, sizeof(normals[0]), sizeof(normals[0])};
		if (tangents) streams[stream_count++] = {tangents, sizeof(tangents[0]), sizeof(tangents[0])};
		if (colors) streams[stream_count++] = {colors, sizeof(colors[0]), sizeof(colors[0])};
		if (uvs) streams[stream_count++] = {uvs, sizeof(uvs[0]), sizeof(uvs[0])};

		if (!tangents && normals && uvs) {
			if (cfg.mikktspace_tangents) {
				computeTangents(import_mesh.computed_tangents, vertex_count, vertices, normals, uvs, path);
			}
			else {
				computeTangentsSimple(import_mesh.computed_tangents, vertex_count, vertices, uvs);
			}
		}

		import_mesh.unique_vertex_count = (u32)meshopt_generateVertexRemapMulti(import_mesh.geom_indices.begin(), nullptr, vertex_count, vertex_count, streams, stream_count);
		
		if (!normals) {
			computeNormals(import_mesh.computed_normals, vertices, vertex_count, import_mesh.geom_indices.begin(), m_allocator);
			normals = import_mesh.computed_normals.begin();

			if (!tangents && uvs) {
				if (cfg.mikktspace_tangents) {
					computeTangents(import_mesh.computed_tangents, vertex_count, vertices, normals, uvs, path);
				}
				else {
					computeTangentsSimple(import_mesh.computed_tangents, vertex_count, vertices, uvs);
				}
			}
		}
	});
	
	if (cfg.bake_vertex_ao) bakeVertexAO(cfg);

	jobs::forEach(m_meshes.size(), 1, [&](i32 mesh_idx, i32){
		PROFILE_BLOCK("FBXImporter postprocessMeshes #2")
		ImportMesh& import_mesh = m_meshes[mesh_idx];
		import_mesh.vertex_data.clear();
		import_mesh.indices.clear();
	
		const ofbx::Mesh& mesh = *import_mesh.fbx;
	
		int vertex_count = mesh.getVertexCount();
		const ofbx::Vec3* vertices = mesh.getVertices();
		const ofbx::Vec3* normals = mesh.getNormals();
		const ofbx::Vec3* tangents = mesh.getTangents();
		const ofbx::Vec4* colors = cfg.import_vertex_colors ? mesh.getColors() : nullptr;
		const ofbx::Vec2* uvs = mesh.getUVs();

		if (!normals) normals = import_mesh.computed_normals.begin();

		Matrix transform_matrix = Matrix::IDENTITY;
		Matrix geometry_matrix = toLumix(mesh.getGeometricMatrix());
		transform_matrix = toLumix(mesh.getGlobalTransform()) * geometry_matrix;
		if (cancel_mesh_transforms) transform_matrix.setTranslation({0, 0, 0});
		if (cfg.origin != ImportConfig::Origin::SOURCE) {
			const bool bottom = cfg.origin == FBXImporter::ImportConfig::Origin::BOTTOM;
			centerMesh(vertices, vertex_count, bottom, transform_matrix, import_mesh.origin);
		}
		import_mesh.transform_matrix = transform_matrix.inverted();

		const bool flip_handness = doesFlipHandness(transform_matrix);
		if (flip_handness) {
			logError("Mesh ", mesh.name, " in ", path, " flips handness. This is not supported and the mesh will not display correctly.");
		}

		const int vertex_size = getVertexSize(mesh, import_mesh.is_skinned, cfg);
		import_mesh.vertex_data.reserve(import_mesh.unique_vertex_count * vertex_size);

		Array<Skin> skinning(m_allocator);
		if (import_mesh.is_skinned) fillSkinInfo(skinning, import_mesh);

		AABB aabb = {{FLT_MAX, FLT_MAX, FLT_MAX}, {-FLT_MAX, -FLT_MAX, -FLT_MAX}};
		float origin_radius_squared = 0;

		int material_idx = getMaterialIndex(mesh, *import_mesh.fbx_mat);
		ASSERT(material_idx >= 0);

		const int* geom_materials = mesh.getMaterialIndices();
		if (!tangents && import_mesh.computed_tangents.size()) {
			tangents = import_mesh.computed_tangents.begin();
		}
		
		Array<u32> intramat_idx(m_allocator);
		intramat_idx.resize(import_mesh.unique_vertex_count);
		memset(intramat_idx.begin(), 0xff, intramat_idx.byte_size());

		u32 written_idx = 0;
		for (int i = 0; i < vertex_count; ++i) {
			if (geom_materials && geom_materials[i / 3] != material_idx) continue;
			if (intramat_idx[import_mesh.geom_indices[i]] != 0xffFFffFF) continue;

			intramat_idx[import_mesh.geom_indices[i]] = written_idx;
			++written_idx;

			ofbx::Vec3 cp = vertices[i];
			// premultiply control points here, so we can have constantly-scaled meshes without scale in bones
			Vec3 pos = transform_matrix.transformPoint(toLumixVec3(cp)) * cfg.mesh_scale * m_fbx_scale;
			pos = fixOrientation(pos);
			import_mesh.vertex_data.write(pos);

			float sq_len = squaredLength(pos);
			origin_radius_squared = maximum(origin_radius_squared, sq_len);

			aabb.min.x = minimum(aabb.min.x, pos.x);
			aabb.min.y = minimum(aabb.min.y, pos.y);
			aabb.min.z = minimum(aabb.min.z, pos.z);
			aabb.max.x = maximum(aabb.max.x, pos.x);
			aabb.max.y = maximum(aabb.max.y, pos.y);
			aabb.max.z = maximum(aabb.max.z, pos.z);

			if (normals) writePackedVec3(normals[i], transform_matrix, &import_mesh.vertex_data);
			if (uvs) writeUV(uvs[i], &import_mesh.vertex_data);
			if (cfg.bake_vertex_ao) {
				const float ao = import_mesh.computed_ao[i];
				u32 ao8 = u8(clamp(ao * 255.f, 0.f, 255.f) + 0.5f);
				u32 ao32 = ao8 | ao8 << 8 | ao8 << 16 | ao8 << 24;
				import_mesh.vertex_data.write(ao32);
			}
			if (colors) {
				if (cfg.vertex_color_is_ao) {
					const u8 ao[4] = { u8(colors[i].x * 255.f + 0.5f) };
					import_mesh.vertex_data.write(ao);
				} else {
					writeColor(colors[i], &import_mesh.vertex_data);
				}
			}
			if (tangents) writePackedVec3(tangents[i], transform_matrix, &import_mesh.vertex_data);
			if (import_mesh.is_skinned) writeSkin(skinning[i], &import_mesh.vertex_data);
		}

		for (int i = 0; i < vertex_count; ++i) {
			if (geom_materials && geom_materials[i / 3] != material_idx) continue;
			const u32 orig_idx = import_mesh.geom_indices[i];
			if (intramat_idx[orig_idx] != 0xffFFffFF) {
				import_mesh.indices.push(intramat_idx[orig_idx]);
			}
		}

		for (u32 i = 0; i < cfg.lod_count; ++i) {
			if ((cfg.autolod_mask & (1 << i)) == 0) continue;
			if (import_mesh.lod != 0) continue;
			
			import_mesh.autolod_indices[i].create(m_allocator);
			import_mesh.autolod_indices[i]->resize(import_mesh.indices.size());
			const size_t lod_index_count = meshopt_simplify(import_mesh.autolod_indices[i]->begin()
				, import_mesh.indices.begin()
				, import_mesh.indices.size()
				, (const float*)import_mesh.vertex_data.data()
				, u32(import_mesh.vertex_data.size() / vertex_size)
				, vertex_size
				, size_t(import_mesh.indices.size() * cfg.autolod_coefs[i])
				, 9001.f // TODO
				);
			import_mesh.autolod_indices[i]->resize((u32)lod_index_count);
		}

		import_mesh.aabb = aabb;
		import_mesh.origin_radius_squared = origin_radius_squared;
		import_mesh.center_radius_squared = 0;
		const Vec3 center = (aabb.max + aabb.min) * 0.5f;

		const u8* mem = import_mesh.vertex_data.data();
		for (u32 i = 0; i < written_idx; ++i) {
			Vec3 p;
			memcpy(&p, mem, sizeof(p));
			import_mesh.center_radius_squared = maximum(import_mesh.center_radius_squared, squaredLength(p - center));
			mem += vertex_size;
		}
	});

	for (int mesh_idx = m_meshes.size() - 1; mesh_idx >= 0; --mesh_idx)
	{
		if (m_meshes[mesh_idx].indices.empty()) m_meshes.swapAndPop(mesh_idx);
	}
}


static int detectMeshLOD(const FBXImporter::ImportMesh& mesh)
{
	const char* node_name = mesh.fbx->name;
	const char* lod_str = findInsensitive(node_name, "_LOD");
	if (!lod_str)
	{
		char mesh_name[256];
		FBXImporter::getImportMeshName(mesh, mesh_name);
		lod_str = findInsensitive(mesh_name, "_LOD");
		if (!lod_str) return 0;
	}

	lod_str += stringLength("_LOD");

	int lod;
	fromCString(lod_str, lod);

	return lod;
}


void FBXImporter::gatherMeshes(ofbx::IScene* plugin)
{
	PROFILE_FUNCTION();
	int c = m_scene->getMeshCount();
	for (int mesh_idx = 0; mesh_idx < c; ++mesh_idx) {
		const ofbx::Mesh* fbx_mesh = (const ofbx::Mesh*)m_scene->getMesh(mesh_idx);
		const int mat_count = fbx_mesh->getMaterialCount();
		for (int j = 0; j < mat_count; ++j) {
			ImportMesh& mesh = m_meshes.emplace(m_allocator);
			mesh.is_skinned = false;
			const ofbx::Skin* skin = fbx_mesh->getSkin();
			if (skin) {
				for (int i = 0; i < skin->getClusterCount(); ++i) {
					if (skin->getCluster(i)->getIndicesCount() > 0) {
						mesh.is_skinned = true;
						break;
					}
				}
			}
			mesh.fbx = fbx_mesh;
			mesh.fbx_mat = fbx_mesh->getMaterial(j);
			mesh.submesh = mat_count > 1 ? j : -1;
			mesh.lod = detectMeshLOD(mesh);
		}
	}
}


FBXImporter::~FBXImporter()
{
	if (m_scene) m_scene->destroy();
	if (m_impostor_shadow_shader) m_impostor_shadow_shader->decRefCount();
}


FBXImporter::FBXImporter(StudioApp& app)
	: m_allocator(app.getAllocator())
	, m_compiler(app.getAssetCompiler())
	, m_scene(nullptr)
	, m_materials(m_allocator)
	, m_meshes(m_allocator)
	, m_animations(m_allocator)
	, m_bones(m_allocator)
	, m_bind_pose(m_allocator)
	, out_file(m_allocator)
	, m_filesystem(app.getEngine().getFileSystem())
	, m_app(app)
	, m_material_name_map(m_allocator)
{
}


static void ofbx_job_processor(ofbx::JobFunction fn, void*, void* data, u32 size, u32 count) {
	jobs::forEach(count, 1, [data, size, fn](i32 i, i32){
		PROFILE_BLOCK("ofbx job");
		u8* ptr = (u8*)data;
		fn(ptr + i * size);
	});
}

void FBXImporter::init() {
	m_impostor_shadow_shader = m_app.getEngine().getResourceManager().load<Shader>(Path("pipelines/impostor_shadow.shd"));
}

bool FBXImporter::setSource(const Path& filename, bool ignore_geometry, bool force_skinned)
{
	out_file.reserve(1024 * 1024);
	PROFILE_FUNCTION();
	if (m_scene) {
		PROFILE_BLOCK("clear previous data");
		m_scene->destroy();
		m_scene = nullptr;	
		m_meshes.clear();
		m_materials.clear();
		m_material_name_map.clear();
		m_animations.clear();
		m_bones.clear();
		m_bind_pose.clear();
	}

	OutputMemoryStream data(m_allocator);
	{
		PROFILE_BLOCK("load file");
		if (!m_filesystem.getContentSync(Path(filename), data)) return false;
	}
	
	const ofbx::LoadFlags flags = ignore_geometry ? ofbx::LoadFlags::IGNORE_GEOMETRY : ofbx::LoadFlags::TRIANGULATE;
	{
		PROFILE_BLOCK("ofbx::load");
		m_scene = ofbx::load(data.data(), (i32)data.size(), static_cast<u16>(flags), &ofbx_job_processor, nullptr);
	}
	if (!m_scene)
	{
		logError("Failed to import \"", filename, ": ", ofbx::getError(), "\n"
			"Please try to convert the FBX file with Autodesk FBX Converter or some other software to the latest version.");
		return false;
	}
	m_fbx_scale = m_scene->getGlobalSettings()->UnitScaleFactor * 0.01f;

	const ofbx::GlobalSettings* settings = m_scene->getGlobalSettings();
	switch (settings->UpAxis) {
		case ofbx::UpVector_AxisX: m_orientation = Orientation::X_UP; break;
		case ofbx::UpVector_AxisY: m_orientation = Orientation::Y_UP; break;
		case ofbx::UpVector_AxisZ: m_orientation = Orientation::Z_UP; break;
	}

	StringView src_dir = Path::getDir(filename);
	if (!ignore_geometry) extractEmbedded(*m_scene, src_dir);
	gatherMeshes(m_scene);

	gatherAnimations(*m_scene);
	if (!ignore_geometry) {
		gatherMaterials(filename, src_dir);
		m_materials.removeDuplicates([](const ImportMaterial& a, const ImportMaterial& b) { return a.fbx == b.fbx; });
		
		bool any_skinned = false;
		for (const ImportMesh& m : m_meshes) any_skinned = any_skinned || m.is_skinned;
		gatherBones(*m_scene, force_skinned || any_skinned);
	}

	return true;
}

void FBXImporter::writeString(const char* str) { out_file.write(str, stringLength(str)); }
	
static Vec3 impostorToWorld(Vec2 uv) {
	uv = uv * 2 - 1;
	Vec3 position= Vec3(
		uv.x + uv.y,
		0.f,
		uv.x - uv.y
	) * 0.5f;

	position.y = -(1.f - fabsf(position.x) - fabsf(position.z));
	return position;
};

static constexpr u32 IMPOSTOR_TILE_SIZE = 512;
static constexpr u32 IMPOSTOR_COLS = 9;

static void getBBProjection(const AABB& aabb, Vec2& out_min, Vec2& out_max) {
	const float radius = length(aabb.max - aabb.min) * 0.5f;
	const Vec3 center = (aabb.min + aabb.max) * 0.5f;

	Matrix proj;
	proj.setOrtho(-1, 1, -1, 1, 0, radius * 2, true);
	Vec2 min(FLT_MAX, FLT_MAX), max(-FLT_MAX, -FLT_MAX);
	for (u32 j = 0; j < IMPOSTOR_COLS; ++j) {
		for (u32 i = 0; i < IMPOSTOR_COLS; ++i) {
			const Vec3 v = impostorToWorld({i / (float)(IMPOSTOR_COLS - 1), j / (float)(IMPOSTOR_COLS - 1)});
			Matrix view;
			view.lookAt(center + v, center, Vec3(0, 1, 0));
			const Matrix vp = proj * view;
			for (u32 k = 0; k < 8; ++k) {
				const Vec3 p = {
					k & 1 ? aabb.min.x : aabb.max.x,
					k & 2 ? aabb.min.y : aabb.max.y,
					k & 4 ? aabb.min.z : aabb.max.z
				};
				const Vec4 proj_p = vp * Vec4(p, 1);
				min.x = minimum(min.x, proj_p.x / proj_p.w);
				min.y = minimum(min.y, proj_p.y / proj_p.w);
				max.x = maximum(max.x, proj_p.x / proj_p.w);
				max.y = maximum(max.y, proj_p.y / proj_p.w);
			}
		}
	}
	out_min = min;
	out_max = max;
}


bool FBXImporter::createImpostorTextures(Model* model, Array<u32>& gb0_rgba, Array<u32>& gb1_rgba, Array<u16>& gb_depth, Array<u32>& shadow_data, IVec2& tile_size, bool bake_normals)
{
	ASSERT(model->isReady());
	ASSERT(m_impostor_shadow_shader->isReady());

	Engine& engine = m_app.getEngine();
	Renderer* renderer = (Renderer*)engine.getSystemManager().getSystem("renderer");
	ASSERT(renderer);

	const u32 capture_define = 1 << renderer->getShaderDefineIdx("DEFERRED");
	const u32 bake_normals_define = 1 << renderer->getShaderDefineIdx("BAKE_NORMALS");

	renderer->pushJob("create impostor textures", [&](DrawStream& stream) {
		const AABB aabb = model->getAABB();
		const float radius = model->getCenterBoundingRadius();

		gpu::TextureHandle gbs[] = {gpu::allocTextureHandle(), gpu::allocTextureHandle(), gpu::allocTextureHandle()};

		const Vec3 center = Vec3(0, (aabb.min + aabb.max).y * 0.5f, 0);
		Vec2 min, max;
		getBBProjection(aabb, min, max);
		if (max.x > radius && min.y < -radius && max.y > radius && min.y < radius) {
			max = Vec2(radius);
			min = Vec2(-radius);
		}
		const Vec2 padding = Vec2(1.f) / Vec2(IMPOSTOR_TILE_SIZE) * (max - min);
		min += -padding;
		max += padding;
		const Vec2 size = max - min;

		tile_size = IVec2(int(IMPOSTOR_TILE_SIZE * size.x / size.y), IMPOSTOR_TILE_SIZE);
		tile_size.x = (tile_size.x + 3) & ~3;
		tile_size.y = (tile_size.y + 3) & ~3;
		const IVec2 texture_size = tile_size * IMPOSTOR_COLS;
		stream.createTexture(gbs[0], texture_size.x, texture_size.y, 1, gpu::TextureFormat::SRGBA, gpu::TextureFlags::NO_MIPS | gpu::TextureFlags::RENDER_TARGET, "impostor_gb0");
		stream.createTexture(gbs[1], texture_size.x, texture_size.y, 1, gpu::TextureFormat::RGBA8, gpu::TextureFlags::NO_MIPS | gpu::TextureFlags::RENDER_TARGET, "impostor_gb1");
		stream.createTexture(gbs[2], texture_size.x, texture_size.y, 1, gpu::TextureFormat::D32, gpu::TextureFlags::NO_MIPS | gpu::TextureFlags::RENDER_TARGET, "impostor_gbd");

		stream.setFramebuffer(gbs, 2, gbs[2], gpu::FramebufferFlags::SRGB);
		const float color[] = {0, 0, 0, 0};
		stream.clear(gpu::ClearFlags::COLOR | gpu::ClearFlags::DEPTH | gpu::ClearFlags::STENCIL, color, 0);

		PassState pass_state;
		pass_state.view = Matrix::IDENTITY;
		pass_state.projection.setOrtho(min.x, max.x, min.y, max.y, 0, 2.02f * radius, true);
		pass_state.inv_projection = pass_state.projection.inverted();
		pass_state.inv_view = pass_state.view.fastInverted();
		pass_state.view_projection = pass_state.projection * pass_state.view;
		pass_state.inv_view_projection = pass_state.view_projection.inverted();
		pass_state.view_dir = Vec4(pass_state.view.inverted().transformVector(Vec3(0, 0, -1)), 0);
		pass_state.camera_up = Vec4(pass_state.view.inverted().transformVector(Vec3(0, 1, 0)), 0);
		const Renderer::TransientSlice pass_buf = renderer->allocUniform(&pass_state, sizeof(pass_state));
		stream.bindUniformBuffer(UniformBuffer::PASS, pass_buf.buffer, pass_buf.offset, pass_buf.size);

		for (u32 j = 0; j < IMPOSTOR_COLS; ++j) {
			for (u32 col = 0; col < IMPOSTOR_COLS; ++col) {
				if (gpu::isOriginBottomLeft()) {
					stream.viewport(col * tile_size.x, j * tile_size.y, tile_size.x, tile_size.y);
				} else {
					stream.viewport(col * tile_size.x, (IMPOSTOR_COLS - j - 1) * tile_size.y, tile_size.x, tile_size.y);
				}
				const Vec3 v = normalize(impostorToWorld({col / (float)(IMPOSTOR_COLS - 1), j / (float)(IMPOSTOR_COLS - 1)}));

				Matrix model_mtx;
				Vec3 up = Vec3(0, 1, 0);
				if (col == IMPOSTOR_COLS >> 1 && j == IMPOSTOR_COLS >> 1) up = Vec3(1, 0, 0);
				model_mtx.lookAt(center - v * 1.01f * radius, center, up);
				const Renderer::TransientSlice ub = renderer->allocUniform(&model_mtx, sizeof(model_mtx));
				stream.bindUniformBuffer(UniformBuffer::DRAWCALL, ub.buffer, ub.offset, ub.size);

				for (u32 i = 0; i <= (u32)model->getLODIndices()[0].to; ++i) {
					const Mesh& mesh = model->getMesh(i);
					Shader* shader = mesh.material->getShader();
					const Material* material = mesh.material;
					const gpu::StateFlags state = gpu::StateFlags::DEPTH_FN_GREATER | gpu::StateFlags::DEPTH_WRITE | material->m_render_states;
					const gpu::ProgramHandle program = shader->getProgram(state, mesh.vertex_decl, capture_define | material->getDefineMask());

					stream.bind(0, material->m_bind_group);
					stream.useProgram(program);
					stream.bindIndexBuffer(mesh.index_buffer_handle);
					stream.bindVertexBuffer(0, mesh.vertex_buffer_handle, 0, mesh.vb_stride);
					stream.bindVertexBuffer(1, gpu::INVALID_BUFFER, 0, 0);
					stream.drawIndexed(0, mesh.indices_count, mesh.index_type);
				}
			}
		}

		stream.setFramebuffer(nullptr, 0, gpu::INVALID_TEXTURE, gpu::FramebufferFlags::NONE);

		gb0_rgba.resize(texture_size.x * texture_size.y);
		gb1_rgba.resize(gb0_rgba.size());
		gb_depth.resize(gb0_rgba.size());
		shadow_data.resize(gb0_rgba.size());

		gpu::TextureHandle shadow = gpu::allocTextureHandle();
		stream.createTexture(shadow, texture_size.x, texture_size.y, 1, gpu::TextureFormat::RGBA8, gpu::TextureFlags::NO_MIPS | gpu::TextureFlags::COMPUTE_WRITE, "impostor_shadow");
		gpu::ProgramHandle shadow_program = m_impostor_shadow_shader->getProgram(bake_normals ? bake_normals_define : 0);
		stream.useProgram(shadow_program);
		stream.bindImageTexture(shadow, 0);
		stream.bindTextures(&gbs[1], 1, 2);
		struct {
			Matrix projection;
			Matrix proj_to_model;
			Matrix inv_view;
			Vec4 center;
			IVec2 tile;
			IVec2 tile_size;
			int size;
			float radius;
		} data;
		for (u32 j = 0; j < IMPOSTOR_COLS; ++j) {
			for (u32 i = 0; i < IMPOSTOR_COLS; ++i) {
				Matrix view, projection;
				const Vec3 v = normalize(impostorToWorld({i / (float)(IMPOSTOR_COLS - 1), j / (float)(IMPOSTOR_COLS - 1)}));
				Vec3 up = Vec3(0, 1, 0);
				if (i == IMPOSTOR_COLS >> 1 && j == IMPOSTOR_COLS >> 1) up = Vec3(1, 0, 0);
				view.lookAt(center - v * 1.01f * radius, center, up);
				projection.setOrtho(min.x, max.x, min.y, max.y, 0, 2.02f * radius, true);
				data.proj_to_model = (projection * view).inverted();
				data.projection = projection;
				data.inv_view = view.inverted();
				data.center = Vec4(center, 1);
				data.tile = IVec2(i, j);
				data.tile_size = tile_size;
				data.size = IMPOSTOR_COLS;
				data.radius = radius;
				const Renderer::TransientSlice ub = renderer->allocUniform(&data, sizeof(data));
				stream.bindUniformBuffer(UniformBuffer::DRAWCALL, ub.buffer, ub.offset, ub.size);
				stream.dispatch((tile_size.x + 15) / 16, (tile_size.y + 15) / 16, 1);
			}
		}

		gpu::TextureHandle staging = gpu::allocTextureHandle();
		const gpu::TextureFlags flags = gpu::TextureFlags::NO_MIPS | gpu::TextureFlags::READBACK;
		stream.createTexture(staging, texture_size.x, texture_size.y, 1, gpu::TextureFormat::RGBA8, flags, "staging_buffer");
		stream.copy(staging, gbs[0], 0, 0);
		stream.readTexture(staging, 0, Span((u8*)gb0_rgba.begin(), gb0_rgba.byte_size()));

		stream.copy(staging, gbs[1], 0, 0);
		stream.readTexture(staging, 0, Span((u8*)gb1_rgba.begin(), gb1_rgba.byte_size()));

		stream.copy(staging, shadow, 0, 0);
		stream.readTexture(staging, 0, Span((u8*)shadow_data.begin(), shadow_data.byte_size()));
		stream.destroy(staging);

		{
			gpu::TextureHandle staging_depth = gpu::allocTextureHandle();
			stream.createTexture(staging_depth, texture_size.x, texture_size.y, 1, gpu::TextureFormat::D32, flags, "staging_buffer");
			stream.copy(staging_depth, gbs[2], 0, 0);
			Array<u32> tmp(m_allocator);
			tmp.resize(gb_depth.size());
			stream.readTexture(staging_depth, 0, Span((u8*)tmp.begin(), tmp.byte_size()));
			for (i32 i = 0; i < tmp.size(); ++i) {
				gb_depth[i] = u16(0xffFF - (tmp[i] >> 16));
			}
			stream.destroy(staging_depth);
		}

		stream.destroy(shadow);
		stream.destroy(gbs[0]);
		stream.destroy(gbs[1]);
		stream.destroy(gbs[2]);
	});

	renderer->frame();
	renderer->waitForRender();

	const PathInfo src_info(model->getPath());
	const Path mat_src(src_info.dir, src_info.basename, "_impostor.mat");
	os::OutputFile f;
	if (!m_filesystem.fileExists(mat_src)) {
		if (!m_filesystem.open(mat_src, f)) {
			logError("Failed to create ", mat_src);
		}
		else {
			const AABB& aabb = model->getAABB();
			const Vec3 center = (aabb.max + aabb.min) * 0.5f;
			f << "shader \"/pipelines/impostor.shd\"\n";
			f << "texture \"" << src_info.basename << "_impostor0.tga\"\n";
			f << "texture \"\"\n";
			f << "texture \"" << src_info.basename << "_impostor2.tga\"\n";
			f << "texture \"" << src_info.basename << "_impostor_depth.raw\"\n";
			f << "defines { \"ALPHA_CUTOUT\" }\n";
			f << "layer \"impostor\"\n";
			f << "backface_culling(false)\n";
			f << "uniform(\"Center\", { 0, " << center.y << ", 0 })\n";
			f << "uniform(\"Radius\", " << model->getCenterBoundingRadius() << ")\n";
			f.close();
		}
	}
	
	const Path albedo_meta(src_info.dir, src_info.basename, "_impostor0.tga.meta");
	if (!m_filesystem.fileExists(albedo_meta)) {
		if (!m_filesystem.open(albedo_meta, f)) {
			logError("Failed to create ", albedo_meta);
		}
		else {
			f << "srgb = true";
			f.close();
		}
	}

	return true;
}


void FBXImporter::writeMaterials(const Path& src, const ImportConfig& cfg)
{
	PROFILE_FUNCTION()
	StringView dir = Path::getDir(src);
	for (const ImportMaterial& material : m_materials) {
		if (!material.import) continue;

		const String& mat_name = m_material_name_map[material.fbx];

		const Path mat_src(dir, mat_name, ".mat");

		os::OutputFile f;
		if (!m_filesystem.open(mat_src, f))
		{
			logError("Failed to create ", mat_src);
			continue;
		}
		out_file.clear();

		writeString("shader \"/pipelines/standard.shd\"\n");
		if (material.alpha_cutout) writeString("defines {\"ALPHA_CUTOUT\"}\n");
		if (material.textures[2].is_valid) writeString("uniform(\"Metallic\", 1.000000)");

		auto writeTexture = [this](const ImportTexture& texture, u32 idx) {
			if (texture.is_valid && idx < 2) {
				const Path meta_path(texture.src, ".meta");
				if (!m_filesystem.fileExists(meta_path)) {
					os::OutputFile file;
					if (m_filesystem.open(meta_path, file)) {
						file << (idx == 0 ? "srgb = true\n" : "normalmap = true\n");
						file.close();
					}
				}
			}
			if (texture.fbx && !texture.src.empty()) {
				writeString("texture \"/");
				writeString(texture.src);
				writeString("\"\n");
			}
			else
			{
				writeString("texture \"\"\n");
			}
		};

		writeTexture(material.textures[0], 0);
		writeTexture(material.textures[1], 1);
		writeTexture(material.textures[2], 2);
		
		if (!material.textures[0].fbx) {
			ofbx::Color diffuse_color = material.fbx->getDiffuseColor();
			out_file << "uniform(\"Material color\", {" << powf(diffuse_color.r, 2.2f) 
				<< "," << powf(diffuse_color.g, 2.2f)
				<< "," << powf(diffuse_color.b, 2.2f)
				<< ",1})\n";
		}

		if (!f.write(out_file.data(), out_file.size())) {
			logError("Failed to write ", mat_src);
		}
		f.close();
	}
}

static void convert(const ofbx::Matrix& mtx, Vec3& pos, Quat& rot)
{
	Matrix m = toLumix(mtx);
	m.normalizeScale();
	rot = m.getRotation();
	pos = m.getTranslation();
}

static float evalCurve(i64 time, const ofbx::AnimationCurve& curve) {
	const i64* times = curve.getKeyTime();
	const float* values = curve.getKeyValue();
	const int count = curve.getKeyCount();

	ASSERT(count > 0);

	time = clamp(time, times[0], times[count - 1]);

	for (int i = 0; i < count; ++i) {
		if (time == times[i]) return values[i];
		if (time < times[i]) {
			ASSERT(i > 0);
			ASSERT(time > times[i - 1]);
			const float t = float((time - times[i - 1]) / double(times[i] - times[i - 1]));
			return values[i - 1] * (1 - t) + values[i] * t;
		}
	}
	ASSERT(false);
	return 0.f;
};

static float getScaleX(const ofbx::Matrix& mtx)
{
	Vec3 v(float(mtx.m[0]), float(mtx.m[4]), float(mtx.m[8]));

	return length(v);
}

static i64 sampleToFBXTime(u32 sample, float fps) {
	return ofbx::secondsToFbxTime(sample / fps);
}

static void fill(const ofbx::Object& bone, const ofbx::AnimationLayer& layer, Array<FBXImporter::Key>& keys, u32 from_sample, u32 samples_count, float fps) {
	const ofbx::AnimationCurveNode* translation_node = layer.getCurveNode(bone, "Lcl Translation");
	const ofbx::AnimationCurveNode* rotation_node = layer.getCurveNode(bone, "Lcl Rotation");
	if (!translation_node && !rotation_node) return;

	keys.resize(samples_count);
	
	auto fill_rot = [&](u32 idx, const ofbx::AnimationCurve* curve) {
		if (!curve) {
			const ofbx::Vec3 lcl_rot = bone.getLocalRotation();
			for (FBXImporter::Key& k : keys) {
				(&k.rot.x)[idx] = float((&lcl_rot.x)[idx]);
			}
			return;
		}

		for (u32 f = 0; f < samples_count; ++f) {
			FBXImporter::Key& k = keys[f];
			(&k.rot.x)[idx] = evalCurve(sampleToFBXTime(from_sample + f, fps), *curve);
		}
	};
	
	auto fill_pos = [&](u32 idx, const ofbx::AnimationCurve* curve) {
		if (!curve) {
			const ofbx::Vec3 lcl_pos = bone.getLocalTranslation();
			for (FBXImporter::Key& k : keys) {
				(&k.pos.x)[idx] = float((&lcl_pos.x)[idx]);
			}
			return;
		}

		for (u32 f = 0; f < samples_count; ++f) {
			FBXImporter::Key& k = keys[f];
			(&k.pos.x)[idx] = evalCurve(sampleToFBXTime(from_sample + f, fps), *curve);
		}
	};
	
	fill_rot(0, rotation_node ? rotation_node->getCurve(0) : nullptr);
	fill_rot(1, rotation_node ? rotation_node->getCurve(1) : nullptr);
	fill_rot(2, rotation_node ? rotation_node->getCurve(2) : nullptr);

	fill_pos(0, translation_node ? translation_node->getCurve(0) : nullptr);
	fill_pos(1, translation_node ? translation_node->getCurve(1) : nullptr);
	fill_pos(2, translation_node ? translation_node->getCurve(2) : nullptr);

	for (FBXImporter::Key& key : keys) {
		const ofbx::Matrix mtx = bone.evalLocal({key.pos.x, key.pos.y, key.pos.z}, {key.rot.x, key.rot.y, key.rot.z});
		convert(mtx, key.pos, key.rot);
	}
}

/*
static bool isBindPoseRotationTrack(u32 count, const Array<FBXImporter::Key>& keys, const Quat& bind_rot, float error) {
	if (count != 2) return false;
	for (const FBXImporter::Key& key : keys) {
		if (key.flags & 1) continue;
		if (fabs(key.rot.x - bind_rot.x) > error) return false;
		if (fabs(key.rot.y - bind_rot.y) > error) return false;
		if (fabs(key.rot.z - bind_rot.z) > error) return false;
		if (fabs(key.rot.w - bind_rot.w) > error) return false;
	}
	return true;
}
*/
static bool isBindPosePositionTrack(u32 count, const Array<FBXImporter::Key>& keys, const Vec3& bind_pos) {
	const float ERROR = 0.00001f;
	for (const FBXImporter::Key& key : keys) {
		const Vec3 d = key.pos - bind_pos;
		if (fabsf(d.x) > ERROR || fabsf(d.y) > ERROR || fabsf(d.z) > ERROR) return false;
	}
	return true;
}

namespace {

struct BitWriter {
	BitWriter(OutputMemoryStream& blob, u32 total_bits)
		: blob(blob)
	{
		const u64 offset = blob.size();
		blob.resize(blob.size() + (total_bits + 7) / 8);
		ptr = blob.getMutableData() + offset;
		memset(ptr, 0, (total_bits + 7) / 8);
	}

	static u32 quantize(float v, float min, float max, u32 bitsize) {
		return u32(double(v - min) / (max - min) * (1 << bitsize) + 0.5f);
	}

	void write(float v, float min, float max, u32 bitsize) {
		ASSERT(bitsize < 32);
		write(quantize(v, min, max, bitsize), bitsize);
	}

	void write(u64 v, u32 bitsize) {
		u64 tmp;
		memcpy(&tmp, &ptr[cursor / 8], sizeof(tmp));
		tmp |= v << (cursor & 7);
		memcpy(&ptr[cursor / 8], &tmp, sizeof(tmp));
		cursor += bitsize;
	};

	OutputMemoryStream& blob;
	u32 cursor = 0;
	u8* ptr;
};

struct TranslationTrack {
	Vec3 min, max;
	u8 bitsizes[4] = {};
	bool is_const = false;
};

struct RotationTrack {
	Quat min, max;
	u8 bitsizes[4];
	bool is_const;
	u8 skipped_channel;
};

u64 pack(float v, float min, float range, u32 bitsize) {
	double normalized = double(v - min) / range;
	return u64(normalized * double((1 << bitsize) - 1) + 0.5f);
}

u64 pack(const Quat& r, const RotationTrack& track) {
	u64 res = 0;
	if (track.skipped_channel != 3) {
		res |= pack(r.w, track.min.w, track.max.w - track.min.w, track.bitsizes[3]);
	}
	
	if (track.skipped_channel != 2) {
		res <<= track.bitsizes[2];
		res |= pack(r.z, track.min.z, track.max.z - track.min.z, track.bitsizes[2]);
	}

	if (track.skipped_channel != 1) {
		res <<= track.bitsizes[1];
		res |= pack(r.y, track.min.y, track.max.y - track.min.y, track.bitsizes[1]);
	}

	if (track.skipped_channel != 0) {
		res <<= track.bitsizes[0];
		res |= pack(r.x, track.min.x, track.max.x - track.min.x, track.bitsizes[0]);
	}
	return res;
}

u64 pack(const Vec3& p, const TranslationTrack& track) {
	u64 res = 0;
	res |= pack(p.z, track.min.z, track.max.z - track.min.z, track.bitsizes[2]);
	res <<= track.bitsizes[1];

	res |= pack(p.y, track.min.y, track.max.y - track.min.y, track.bitsizes[1]);
	res <<= track.bitsizes[0];

	res |= pack(p.x, track.min.x, track.max.x - track.min.x, track.bitsizes[0]);
	return res;
}

bool clampBitsizes(Span<u8> values) {
	u32 total = 0;
	for (u8 v : values) total += v;
	if (total > 64) {
		u32 over = total - 64;
		u32 i =  0;
		while (over) {
			if (values[i] > 0) {
				--values[i];
				--over;
			}
			i = (i + 1) % values.length();
		}
		
		return true;
	}
	return false;
}

}

void FBXImporter::writeAnimations(const Path& src, const ImportConfig& cfg)
{
	PROFILE_FUNCTION();
	for (const FBXImporter::ImportAnimation& anim : m_animations) { 
		ASSERT(anim.import);

		const ofbx::AnimationStack* stack = anim.fbx;
		const ofbx::AnimationLayer* layer = stack->getLayer(0);
		ASSERT(anim.scene == m_scene);
		const float fps = m_scene->getSceneFrameRate();
		const ofbx::TakeInfo* take_info = m_scene->getTakeInfo(stack->name);
		if(!take_info && startsWith(stack->name, "AnimStack::")) {
			take_info = m_scene->getTakeInfo(stack->name + 11);
		}

		double full_len;
		if (take_info) {
			full_len = take_info->local_time_to - take_info->local_time_from;
		}
		else if(m_scene->getGlobalSettings()) {
			full_len = m_scene->getGlobalSettings()->TimeSpanStop;
		}
		else {
			logError("Unsupported animation in ", src);
			continue;
		}

		Array<TranslationTrack> translation_tracks(m_allocator);
		Array<RotationTrack> rotation_tracks(m_allocator);
		translation_tracks.resize(m_bones.size());
		rotation_tracks.resize(m_bones.size());

		auto write_animation = [&](StringView name, u32 from_sample, u32 samples_count) {
			out_file.clear();
			Animation::Header header;
			header.magic = Animation::HEADER_MAGIC;
			header.version = Animation::Version::LAST;
			write(header);
			write(fps);
			write(samples_count - 1);
			write(cfg.animation_flags);

			Array<Array<Key>> all_keys(m_allocator);

			all_keys.reserve(m_bones.size());
			for (const ofbx::Object* bone : m_bones) {
				Array<Key>& keys = all_keys.emplace(m_allocator);
				fill(*bone, *layer, keys, from_sample, samples_count, fps);
			}

			for (const ofbx::Object*& bone : m_bones) {
				ofbx::Object* parent = bone->getParent();
				if (!parent) continue;

				// parent_scale - animated scale is not supported, but we can get rid of static scale if we ignore
				// it in writeSkeleton() and use `parent_scale` in this function
				const float parent_scale = (float)getScaleX(parent->getGlobalTransform());
				Array<Key>& keys = all_keys[u32(&bone - m_bones.begin())];
				for (Key& k : keys) k.pos *= parent_scale;
			}

			{
				u32 total_bits = 0;
				u32 translation_curves_count = 0;
				u64 toffset = out_file.size();
				u16 offset_bits = 0;
				write(translation_curves_count);
				for (const ofbx::Object*& bone : m_bones) {
					Array<Key>& keys = all_keys[u32(&bone - m_bones.begin())];
					if (keys.empty()) continue;

					const u32 bone_idx = u32(&bone - m_bones.begin());

					ofbx::Object* parent = bone->getParent();
					Vec3 bind_pos;
					if (!parent) {
						bind_pos = m_bind_pose[bone_idx].getTranslation();
					}
					else {
						const int parent_idx = m_bones.indexOf(parent);
						if (m_bind_pose.empty()) {
							// TODO should not we evalLocal here like in rotation ~50lines below?
							bind_pos = toLumixVec3(bone->getLocalTranslation());
						}
						else {
							bind_pos = (m_bind_pose[parent_idx].inverted() * m_bind_pose[bone_idx]).getTranslation();
						}
					}

					if (isBindPosePositionTrack(keys.size(), keys, bind_pos)) continue;
			
					const BoneNameHash name_hash(bone->name);
					write(name_hash);

					Vec3 min(FLT_MAX), max(-FLT_MAX);
					for (const Key& k : keys) {
						const Vec3 p = fixOrientation(k.pos * cfg.mesh_scale * m_fbx_scale);
						min = minimum(p, min);
						max = maximum(p, max);
					}
					const u8 bitsizes[] = {
						(u8)log2(u32((max.x - min.x) / 0.00005f / cfg.anim_translation_error)),
						(u8)log2(u32((max.y - min.y) / 0.00005f / cfg.anim_translation_error)),
						(u8)log2(u32((max.z - min.z) / 0.00005f / cfg.anim_translation_error))
					};
					const u8 bitsize = (bitsizes[0] + bitsizes[1] + bitsizes[2]);

					if (bitsize == 0) {
						translation_tracks[bone_idx].is_const = true;
						write(Animation::TrackType::CONSTANT);
						write(keys[0].pos * cfg.mesh_scale * m_fbx_scale);
					}
					else {
						translation_tracks[bone_idx].is_const = false;
						write(Animation::TrackType::ANIMATED);

						write(min);
						write((max.x - min.x) / ((1 << bitsizes[0]) - 1));
						write((max.y - min.y) / ((1 << bitsizes[1]) - 1));
						write((max.z - min.z) / ((1 << bitsizes[2]) - 1));
						write(bitsizes);
						write(offset_bits);
						offset_bits += bitsize;
				
						memcpy(translation_tracks[bone_idx].bitsizes, bitsizes, sizeof(bitsizes));
						translation_tracks[bone_idx].max = max;
						translation_tracks[bone_idx].min = min;
						total_bits += bitsize * keys.size();
					}				

					++translation_curves_count;
				}

				BitWriter bit_writer(out_file, total_bits);

				for (u32 i = 0; i < samples_count; ++i) {
					for (const ofbx::Object*& bone : m_bones) {
						const u32 bone_idx = u32(&bone - m_bones.begin());
						Array<Key>& keys = all_keys[bone_idx];
						const TranslationTrack& track = translation_tracks[bone_idx];

						if (!keys.empty() && !track.is_const) {
							const Key& k = keys[i];
							Vec3 p = fixOrientation(k.pos * cfg.mesh_scale * m_fbx_scale);
							const u64 packed = pack(p, track);
							const u32 bitsize = (track.bitsizes[0] + track.bitsizes[1] + track.bitsizes[2]);
							ASSERT(bitsize <= 64);
							bit_writer.write(packed, bitsize);
						}
					}
				}

				memcpy(out_file.getMutableData() + toffset, &translation_curves_count, sizeof(translation_curves_count));
			}

			u32 rotation_curves_count = 0;
			u64 roffset = out_file.size();
			write(rotation_curves_count);

			u32 total_bits = 0;
			u16 offset_bits = 0;
			for (const ofbx::Object*& bone : m_bones) {
				Array<Key>& keys = all_keys[u32(&bone - m_bones.begin())];
				if (keys.empty()) continue;
			
				Quat bind_rot;
				const u32 bone_idx = u32(&bone - m_bones.begin());
				ofbx::Object* parent = bone->getParent();
				if (!parent)
				{
					bind_rot = m_bind_pose[bone_idx].getRotation();
				}
				else
				{
					const i32 parent_idx = m_bones.indexOf(parent);
					if (m_bind_pose.empty()) {
						const Matrix mtx = toLumix(bone->evalLocal(bone->getLocalTranslation(), bone->getLocalRotation()));
						bind_rot = mtx.getRotation();
					}
					else {
						bind_rot = (m_bind_pose[parent_idx].inverted() * m_bind_pose[bone_idx]).getRotation();
					}
				}

				//if (isBindPoseRotationTrack(count, keys, bind_rot, cfg.rotation_error)) continue;

				const BoneNameHash name_hash(bone->name);
				write(name_hash);

				Quat min(FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX), max(-FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX);
				for (const Key& k : keys) {
					const Quat r = fixOrientation(k.rot);
					min.x = minimum(min.x, r.x); max.x = maximum(max.x, r.x);
					min.y = minimum(min.y, r.y); max.y = maximum(max.y, r.y);
					min.z = minimum(min.z, r.z); max.z = maximum(max.z, r.z);
					min.w = minimum(min.w, r.w); max.w = maximum(max.w, r.w);
				}
				
				u8 bitsizes[] = {
					(u8)log2(u32((max.x - min.x) / 0.000001f / cfg.anim_rotation_error)),
					(u8)log2(u32((max.y - min.y) / 0.000001f / cfg.anim_rotation_error)),
					(u8)log2(u32((max.z - min.z) / 0.000001f / cfg.anim_rotation_error)),
					(u8)log2(u32((max.w - min.w) / 0.000001f / cfg.anim_rotation_error))
				};
				if (clampBitsizes(bitsizes)) {
					logWarning("Clamping bone ", bone->name, " in ", src);
				}

				if (bitsizes[0] + bitsizes[1] + bitsizes[2] + bitsizes[3] == 0) {
					rotation_tracks[bone_idx].is_const = true;
					write(Animation::TrackType::CONSTANT);
					write(keys[0].rot);
				}
				else {
					rotation_tracks[bone_idx].is_const = false;
					write(Animation::TrackType::ANIMATED);

					u8 skipped_channel = 0;
					for (u32 i = 1; i < 4; ++i) {
						if (bitsizes[i] > bitsizes[skipped_channel]) skipped_channel = i;
					}

					for (u32 i = 0; i < 4; ++i) {
						if (skipped_channel == i) continue;
						write((&min.x)[i]);
					}
					for (u32 i = 0; i < 4; ++i) {
						if (skipped_channel == i) continue;
						write(((&max.x)[i] - (&min.x)[i]) / ((1 << bitsizes[i]) - 1));
					}
					for (u32 i = 0; i < 4; ++i) {
						if (skipped_channel == i) continue;
						write(bitsizes[i]);
					}
					u8 bitsize = bitsizes[0] + bitsizes[1] + bitsizes[2] + bitsizes[3] + 1;
					bitsize -= bitsizes[skipped_channel];
					write(offset_bits);
					write(skipped_channel);

					offset_bits += bitsize;
					ASSERT(bitsize > 0 && bitsize <= 64);
				
					memcpy(rotation_tracks[bone_idx].bitsizes, bitsizes, sizeof(bitsizes));
					rotation_tracks[bone_idx].max = max;
					rotation_tracks[bone_idx].min = min;
					rotation_tracks[bone_idx].skipped_channel = skipped_channel;
					total_bits += bitsize * keys.size();
				}
				++rotation_curves_count;
			}
			memcpy(out_file.getMutableData() + roffset, &rotation_curves_count, sizeof(rotation_curves_count));

			BitWriter bit_writer(out_file, total_bits);

			for (u32 i = 0; i < samples_count; ++i) {
				for (const ofbx::Object*& bone : m_bones) {
					const u32 bone_idx = u32(&bone - m_bones.begin());
					Array<Key>& keys = all_keys[bone_idx];
					const RotationTrack& track = rotation_tracks[bone_idx];

					if (!keys.empty() && !track.is_const) {
						const Key& k = keys[i];
						Quat q = fixOrientation(k.rot);
						u32 bitsize = (track.bitsizes[0] + track.bitsizes[1] + track.bitsizes[2] + track.bitsizes[3]);
						bitsize -= track.bitsizes[track.skipped_channel];
						++bitsize; // sign bit
						ASSERT(bitsize <= 64);
						u64 packed = pack(q, track);
						packed <<= 1;
						packed |= (&q.x)[track.skipped_channel] < 0 ? 1 : 0;
						bit_writer.write(packed, bitsize);
					}
				}
			}

			Path anim_path(name, ".ani:", src);
			m_compiler.writeCompiledResource(anim_path, Span(out_file.data(), (i32)out_file.size()));
		};
		if (cfg.clips.length() == 0) {
			write_animation(anim.name, 0, u32(full_len * fps + 0.5f) + 1);
		}
		else {
			for (const ImportConfig::Clip& clip : cfg.clips) {
				write_animation(clip.name, clip.from_frame, clip.to_frame - clip.from_frame + 1);
			}
		}
	}
}

int FBXImporter::getVertexSize(const ofbx::Mesh& mesh, bool is_skinned, const ImportConfig& cfg) const
{
	static const int POSITION_SIZE = sizeof(float) * 3;
	static const int NORMAL_SIZE = sizeof(u8) * 4;
	static const int TANGENT_SIZE = sizeof(u8) * 4;
	static const int UV_SIZE = sizeof(float) * 2;
	static const int COLOR_SIZE = sizeof(u8) * 4;
	static const int AO_SIZE = sizeof(u8) * 4;
	static const int BONE_INDICES_WEIGHTS_SIZE = sizeof(float) * 4 + sizeof(u16) * 4;
	int size = POSITION_SIZE + NORMAL_SIZE;

	if (mesh.getUVs()) size += UV_SIZE;
	if (cfg.bake_vertex_ao) size += AO_SIZE;
	if (mesh.getColors() && cfg.import_vertex_colors) size += cfg.vertex_color_is_ao ? AO_SIZE : COLOR_SIZE;
	if (hasTangents(mesh)) size += TANGENT_SIZE;
	if (is_skinned) size += BONE_INDICES_WEIGHTS_SIZE;

	return size;
}


void FBXImporter::fillSkinInfo(Array<Skin>& skinning, const ImportMesh& import_mesh) const
{
	const ofbx::Mesh* mesh = import_mesh.fbx;
	skinning.resize(mesh->getVertexCount());
	memset(&skinning[0], 0, skinning.size() * sizeof(skinning[0]));

	const ofbx::Skin* fbx_skin = mesh->getSkin();
	if(!fbx_skin) {
		ASSERT(import_mesh.bone_idx >= 0);
		skinning.resize(mesh->getIndexCount());
		for (Skin& skin : skinning) {
			skin.count = 1;
			skin.weights[0] = 1;
			skin.weights[1] = skin.weights[2] = skin.weights[3] = 0;
			skin.joints[0] = skin.joints[1] = skin.joints[2] = skin.joints[3] = import_mesh.bone_idx;
		}
		return;
	}

	for (int i = 0, c = fbx_skin->getClusterCount(); i < c; ++i)
	{
		const ofbx::Cluster* cluster = fbx_skin->getCluster(i);
		if (cluster->getIndicesCount() == 0) continue;
		int joint = m_bones.indexOf(cluster->getLink());
		ASSERT(joint >= 0);
		const int* cp_indices = cluster->getIndices();
		const double* weights = cluster->getWeights();
		for (int j = 0; j < cluster->getIndicesCount(); ++j)
		{
			int idx = cp_indices[j];
			float weight = (float)weights[j];
			Skin& s = skinning[idx];
			if (s.count < 4)
			{
				s.weights[s.count] = weight;
				s.joints[s.count] = joint;
				++s.count;
			}
			else
			{
				int min = 0;
				for (int m = 1; m < 4; ++m)
				{
					if (s.weights[m] < s.weights[min]) min = m;
				}

				if (s.weights[min] < weight)
				{
					s.weights[min] = weight;
					s.joints[min] = joint;
				}
			}
		}
	}

	for (Skin& s : skinning)
	{
		float sum = 0;
		for (float w : s.weights) sum += w;
		for (float& w : s.weights) w /= sum;
	}
}


Vec3 FBXImporter::fixOrientation(const Vec3& v) const
{
	switch (m_orientation)
	{
		case Orientation::Y_UP: return Vec3(v.x, v.y, v.z);
		case Orientation::Z_UP: return Vec3(v.x, v.z, -v.y);
		case Orientation::Z_MINUS_UP: return Vec3(v.x, -v.z, v.y);
		case Orientation::X_MINUS_UP: return Vec3(v.y, -v.x, v.z);
		case Orientation::X_UP: return Vec3(-v.y, v.x, v.z);
	}
	ASSERT(false);
	return Vec3(v.x, v.y, v.z);
}


Quat FBXImporter::fixOrientation(const Quat& v) const
{
	switch (m_orientation)
	{
		case Orientation::Y_UP: return Quat(v.x, v.y, v.z, v.w);
		case Orientation::Z_UP: return Quat(v.x, v.z, -v.y, v.w);
		case Orientation::Z_MINUS_UP: return Quat(v.x, -v.z, v.y, v.w);
		case Orientation::X_MINUS_UP: return Quat(v.y, -v.x, v.z, v.w);
		case Orientation::X_UP: return Quat(-v.y, v.x, v.z, v.w);
	}
	ASSERT(false);
	return Quat(v.x, v.y, v.z, v.w);
}

void FBXImporter::writeImpostorVertices(const AABB& aabb)
{
	#pragma pack(1)
		struct Vertex
		{
			Vec3 pos;
			Vec2 uv;
		};
	#pragma pack()

	const Vec3 center = Vec3(0, (aabb.max + aabb.min).y * 0.5f, 0);

	Vec2 min, max;
	getBBProjection(aabb, min, max);

	const Vertex vertices[] = {
		{{center.x + min.x, center.y + min.y, center.z}, {0, 0}},
		{{center.x + min.x, center.y + max.y, center.z}, {0, 1}},
		{{center.x + max.x, center.y + max.y, center.z}, {1, 1}},
		{{center.x + max.x, center.y + min.y, center.z}, {1, 0}}
	};

	const u32 vertex_data_size = sizeof(vertices);
	write(vertex_data_size);
	for (const Vertex& vertex : vertices) {
		write(vertex.pos);
		write(vertex.uv);
	}
}


void FBXImporter::writeGeometry(int mesh_idx, const ImportConfig& cfg)
{
	float origin_radius_squared = 0;
	float center_radius_squared = 0;
	OutputMemoryStream vertices_blob(m_allocator);
	const ImportMesh& import_mesh = m_meshes[mesh_idx];
	
	bool are_indices_16_bit = areIndices16Bit(import_mesh, cfg);
	if (are_indices_16_bit)
	{
		int index_size = sizeof(u16);
		write(index_size);
		write(import_mesh.indices.size());
		for (int i : import_mesh.indices)
		{
			ASSERT(i <= (1 << 16));
			u16 index = (u16)i;
			write(index);
		}
	}
	else
	{
		int index_size = sizeof(import_mesh.indices[0]);
		write(index_size);
		write(import_mesh.indices.size());
		write(&import_mesh.indices[0], sizeof(import_mesh.indices[0]) * import_mesh.indices.size());
	}
	origin_radius_squared = maximum(origin_radius_squared, import_mesh.origin_radius_squared);
	center_radius_squared = maximum(center_radius_squared, import_mesh.center_radius_squared);

	write((i32)import_mesh.vertex_data.size());
	write(import_mesh.vertex_data.data(), import_mesh.vertex_data.size());

	write(sqrtf(origin_radius_squared));
	write(sqrtf(center_radius_squared));
	write(import_mesh.aabb);
}

static bool hasAutoLOD(const FBXImporter::ImportConfig& cfg, u32 idx) {
	return cfg.autolod_mask & (1 << idx);
}

void FBXImporter::writeGeometry(const ImportConfig& cfg)
{
	AABB aabb = {{0, 0, 0}, {0, 0, 0}};
	float origin_radius_squared = 0;
	float center_radius_squared = 0;
	OutputMemoryStream vertices_blob(m_allocator);

	for (u32 lod = 0; lod < cfg.lod_count - (cfg.create_impostor ? 1 : 0); ++lod) {
		for (const ImportMesh& import_mesh : m_meshes)
		{
			if (!import_mesh.import) continue;

			const bool are_indices_16_bit = areIndices16Bit(import_mesh, cfg);
			origin_radius_squared = maximum(origin_radius_squared, import_mesh.origin_radius_squared);
			center_radius_squared = maximum(center_radius_squared, import_mesh.center_radius_squared);
			aabb.merge(import_mesh.aabb);
			
			if (import_mesh.lod == lod && !hasAutoLOD(cfg, lod)) {

				if (are_indices_16_bit)
				{
					const i32 index_size = sizeof(u16);
					write(index_size);
					write(import_mesh.indices.size());
					for (int i : import_mesh.indices)
					{
						ASSERT(i <= (1 << 16));
						u16 index = (u16)i;
						write(index);
					}
				}
				else
				{
					int index_size = sizeof(import_mesh.indices[0]);
					write(index_size);
					write(import_mesh.indices.size());
					write(&import_mesh.indices[0], sizeof(import_mesh.indices[0]) * import_mesh.indices.size());
				}
			}
			else if (import_mesh.lod == 0 && hasAutoLOD(cfg, lod)) {
				const auto& lod_indices = *import_mesh.autolod_indices[lod].get();
				if (are_indices_16_bit) {
					const i32 index_size = sizeof(u16);
					write(index_size);
					write(lod_indices.size());
					for (u32 i : lod_indices)
					{
						ASSERT(i <= (1 << 16));
						u16 index = (u16)i;
						write(index);
					}
				}
				else
				{
					i32 index_size = sizeof(lod_indices[0]);
					write(index_size);
					write(lod_indices.size());
					write(lod_indices.begin(), import_mesh.autolod_indices[lod]->byte_size());
				}
			}
		}
	}

	if (cfg.create_impostor) {
		const int index_size = sizeof(u16);
		write(index_size);
		const u16 indices[] = {0, 1, 2, 0, 2, 3};
		const u32 len = lengthOf(indices);
		write(len);
		write(indices, sizeof(indices));
	}

	for (u32 lod = 0; lod < cfg.lod_count - (cfg.create_impostor ? 1 : 0); ++lod) {
		for (const ImportMesh& import_mesh : m_meshes) {
			if (!import_mesh.import) continue;
			
			if ((import_mesh.lod == lod && !hasAutoLOD(cfg, lod)) || (import_mesh.lod == 0 && hasAutoLOD(cfg, lod))) {
				write((i32)import_mesh.vertex_data.size());
				write(import_mesh.vertex_data.data(), import_mesh.vertex_data.size());
			}
		}
	}
	if (cfg.create_impostor) {
		writeImpostorVertices(aabb);
		const float r = maximum(squaredLength(aabb.max), squaredLength(aabb.min));
		origin_radius_squared = maximum(origin_radius_squared, r);
		center_radius_squared = maximum(center_radius_squared, squaredLength(aabb.max - aabb.min) * 0.5f);

	}

	if (m_meshes.empty()) {
		#if 0
		Matrix transform_matrix = Matrix::IDENTITY;
		Matrix geometry_matrix = toLumix(mesh.getGeometricMatrix());
		transform_matrix = toLumix(mesh.getGlobalTransform()) * geometry_matrix;
		if (cancel_mesh_transforms) transform_matrix.setTranslation({0, 0, 0});
		if (cfg.origin != ImportConfig::Origin::SOURCE) {
			const bool bottom = cfg.origin == FBXImporter::ImportConfig::Origin::BOTTOM;
			centerMesh(vertices, vertex_count, bottom, transform_matrix, import_mesh.origin);
		}
		import_mesh.transform_matrix = transform_matrix.inverted();
#endif
		for (const ofbx::Object* bone : m_bones) {
			const Matrix mtx = toLumix(bone->getGlobalTransform());
			const Vec3 p = mtx.getTranslation() * cfg.mesh_scale * m_fbx_scale;
			origin_radius_squared = maximum(origin_radius_squared, squaredLength(p));
			aabb.addPoint(p);
		}
		center_radius_squared = squaredLength(aabb.max - aabb.min) * 0.5f;
	}

	write(sqrtf(origin_radius_squared) * cfg.bounding_scale);
	write(sqrtf(center_radius_squared) * cfg.bounding_scale);
	write(aabb * cfg.bounding_scale);
}


void FBXImporter::writeImpostorMesh(StringView dir, StringView model_name)
{
	const i32 attribute_count = 2;
	write(attribute_count);

	write(Mesh::AttributeSemantic::POSITION);
	write(gpu::AttributeType::FLOAT);
	write((u8)3);

	write(Mesh::AttributeSemantic::TEXCOORD0);
	write(gpu::AttributeType::FLOAT);
	write((u8)2);

	const Path material_name(dir, model_name, "_impostor.mat");
	u32 length = material_name.length();
	write(length);
	write(material_name.c_str(), length);

	const char* mesh_name = "impostor";
	length = stringLength(mesh_name);
	write(length);
	write(mesh_name, length);
}


void FBXImporter::writeMeshes(const Path& src, int mesh_idx, const ImportConfig& cfg) {
	const PathInfo src_info(src);
	i32 mesh_count = 0;
	if (mesh_idx >= 0) {
		mesh_count = 1;
	}
	else {
		for (ImportMesh& mesh : m_meshes) {
			if (mesh.lod >= cfg.lod_count - (cfg.create_impostor ? 1 : 0)) continue;
			if (mesh.import && (mesh.lod == 0 || !hasAutoLOD(cfg, mesh.lod))) ++mesh_count;
			for (u32 i = 1; i < cfg.lod_count - (cfg.create_impostor ? 1 : 0); ++i) {
				if (mesh.lod == 0 && hasAutoLOD(cfg, i)) ++mesh_count;
			}
		}
		if (cfg.create_impostor) ++mesh_count;
	}
	write(mesh_count);
	
	auto writeMesh = [&](const ImportMesh& import_mesh ) {
			
		const ofbx::Mesh& mesh = *import_mesh.fbx;

		i32 attribute_count = getAttributeCount(import_mesh, cfg);
		write(attribute_count);

		write(Mesh::AttributeSemantic::POSITION);
		write(gpu::AttributeType::FLOAT);
		write((u8)3);
		write(Mesh::AttributeSemantic::NORMAL);
		write(gpu::AttributeType::I8);
		write((u8)4);

		if (mesh.getUVs()) {
			write(Mesh::AttributeSemantic::TEXCOORD0);
			write(gpu::AttributeType::FLOAT);
			write((u8)2);
		}
		if (cfg.bake_vertex_ao) {
			write(Mesh::AttributeSemantic::AO);
			write(gpu::AttributeType::U8);
			write((u8)4); // 1+3 because of padding
		}
		if (mesh.getColors() && cfg.import_vertex_colors) {
			if (cfg.vertex_color_is_ao) {
				write(Mesh::AttributeSemantic::AO);
				write(gpu::AttributeType::U8);
				write((u8)4); // 1+3 because of padding
			}
			else {
				write(Mesh::AttributeSemantic::COLOR0);
				write(gpu::AttributeType::U8);
				write((u8)4);
			}
		}
		if (hasTangents(mesh)) {
			write(Mesh::AttributeSemantic::TANGENT);
			write(gpu::AttributeType::I8);
			write((u8)4);
		}

		if (import_mesh.is_skinned) {
			write(Mesh::AttributeSemantic::INDICES);
			write(gpu::AttributeType::I16);
			write((u8)4);
			write(Mesh::AttributeSemantic::WEIGHTS);
			write(gpu::AttributeType::FLOAT);
			write((u8)4);
		}

		const ofbx::Material* material = import_mesh.fbx_mat;
		const String& mat_name = m_material_name_map[material];
		const Path mat_id(src_info.dir, mat_name, ".mat");
		const i32 len = mat_id.length();
		write(len);
		write(mat_id.c_str(), len);

		char name[256];
		getImportMeshName(import_mesh, name);
		i32 name_len = (i32)stringLength(name);
		write(name_len);
		write(name, stringLength(name));
	};

	if(mesh_idx >= 0) {
		writeMesh(m_meshes[mesh_idx]);
	}
	else {
		for (u32 lod = 0; lod < cfg.lod_count - (cfg.create_impostor ? 1 : 0); ++lod) {
			for (ImportMesh& import_mesh : m_meshes) {
				if (import_mesh.import && import_mesh.lod == lod && !hasAutoLOD(cfg, lod)) writeMesh(import_mesh);
				else if (import_mesh.lod == 0 && import_mesh.import && hasAutoLOD(cfg, lod)) writeMesh(import_mesh);
			}
		}
	}

	if (mesh_idx < 0 && cfg.create_impostor) {
		writeImpostorMesh(src_info.dir, src_info.basename);
	}
}


void FBXImporter::writeSkeleton(const ImportConfig& cfg)
{
	write(m_bones.size());

	u32 idx = 0;
	m_bind_pose.resize(m_bones.size());
	for (const ofbx::Object*& node : m_bones)
	{
		const char* name = node->name;
		int len = (int)stringLength(name);
		write(len);
		writeString(name);

		ofbx::Object* parent = node->getParent();
		if (!parent)
		{
			write((int)-1);
		}
		else
		{
			const int tmp = m_bones.indexOf(parent);
			write(tmp);
		}

		const ImportMesh* mesh = getAnyMeshFromBone(node, int(&node - m_bones.begin()));
		Matrix tr = toLumix(getBindPoseMatrix(mesh, node));
		tr.normalizeScale();
		m_bind_pose[idx] = tr;

		Quat q = fixOrientation(tr.getRotation());
		Vec3 t = fixOrientation(tr.getTranslation());
		write(t * cfg.mesh_scale * m_fbx_scale);
		write(q);
		++idx;
	}
}


void FBXImporter::writeLODs(const ImportConfig& cfg)
{
	i32 lods[4] = {};
	for (auto& mesh : m_meshes) {
		if (!mesh.import) continue;
		if (mesh.lod >= cfg.lod_count - (cfg.create_impostor ? 1 : 0)) continue;

		if (mesh.lod == 0 || !hasAutoLOD(cfg, mesh.lod)) {
			++lods[mesh.lod];
		}
		for (u32 i = 1; i < cfg.lod_count - (cfg.create_impostor ? 1 : 0); ++i) {
			if (mesh.lod == 0 && hasAutoLOD(cfg, i)) {
				++lods[i];
			}
		}
	}

	if (cfg.create_impostor) {
		lods[cfg.lod_count - 1] = 1;
	}

	write(cfg.lod_count);

	u32 to_mesh = 0;
	for (u32 i = 0; i < cfg.lod_count; ++i) {
		to_mesh += lods[i];
		const i32 tmp = to_mesh - 1;
		write((const char*)&tmp, sizeof(tmp));
		float factor = cfg.lods_distances[i] < 0 ? FLT_MAX : cfg.lods_distances[i] * cfg.lods_distances[i];
		write((const char*)&factor, sizeof(factor));
	}
}


int FBXImporter::getAttributeCount(const ImportMesh& mesh, const ImportConfig& cfg) const
{
	int count = 2; // position & normals
	if (mesh.fbx->getUVs()) ++count;
	if (cfg.bake_vertex_ao) ++count;
	if (mesh.fbx->getColors() && cfg.import_vertex_colors) ++count;
	if (hasTangents(*mesh.fbx)) ++count;
	if (mesh.is_skinned) count += 2;
	return count;
}


bool FBXImporter::areIndices16Bit(const ImportMesh& mesh, const ImportConfig& cfg) const
{
	int vertex_size = getVertexSize(*mesh.fbx, mesh.is_skinned, cfg);
	return !(mesh.import && mesh.vertex_data.size() / vertex_size > (1 << 16));
}

void FBXImporter::bakeVertexAO(const ImportConfig& cfg) {
	PROFILE_FUNCTION();

	AABB aabb(Vec3(FLT_MAX), Vec3(-FLT_MAX));
	for (ImportMesh& import_mesh : m_meshes) {
		const ofbx::Mesh* mesh = import_mesh.fbx;
		const i32 vertex_count = mesh->getVertexCount();
		const ofbx::Vec3* vertices = (ofbx::Vec3*)mesh->getVertices();
		
		for (i32 i = 0; i < vertex_count; ++i) {
			aabb.addPoint(toLumixVec3(vertices[i]));
		}
	}

	Voxels voxels(m_allocator);
	voxels.beginRaster(aabb, 64);
	for (ImportMesh& import_mesh : m_meshes) {
		const ofbx::Mesh* mesh = import_mesh.fbx;
		const i32 vertex_count = mesh->getVertexCount();
		const ofbx::Vec3* vertices = (ofbx::Vec3*)mesh->getVertices();
		
		for (i32 i = 0; i < vertex_count; i += 3) {
			voxels.raster(toLumixVec3(vertices[i]), toLumixVec3(vertices[i + 1]), toLumixVec3(vertices[i + 2]));
		}
	}
	voxels.computeAO(32);
	voxels.blurAO();

	for (ImportMesh& import_mesh : m_meshes) {
		const ofbx::Mesh* mesh = import_mesh.fbx;
		const i32 vertex_count = mesh->getVertexCount();
		const ofbx::Vec3* vertices = (ofbx::Vec3*)mesh->getVertices();

		import_mesh.computed_ao.reserve(vertex_count);
		for (i32 i = 0; i < vertex_count; ++i) {
			Vec3 p = toLumixVec3(vertices[i]);
			float ao;
			bool res = voxels.sampleAO(p, &ao);
			ASSERT(res);
			import_mesh.computed_ao.push(ao);
		}
	}
}

void FBXImporter::writeModelHeader()
{
	Model::FileHeader header;
	header.magic = 0x5f4c4d4f;
	header.version = (u32)Model::FileVersion::LATEST;
	write(header);
}


void FBXImporter::writePhysics(const Path& src, const ImportConfig& cfg)
{
	if (m_meshes.empty()) return;
	if (cfg.physics == ImportConfig::Physics::NONE) return;

	out_file.clear();

	PhysicsGeometry::Header header;
	header.m_magic = PhysicsGeometry::HEADER_MAGIC;
	header.m_version = (u32)PhysicsGeometry::Versions::LAST;
	const bool to_convex = cfg.physics == ImportConfig::Physics::CONVEX;
	header.m_convex = (u32)to_convex;
	out_file.write(&header, sizeof(header));


	PhysicsSystem* ps = (PhysicsSystem*)m_app.getEngine().getSystemManager().getSystem("physics");
	if (!ps) {
		logError(src, ": no physics system found while trying to cook physics data");
		return;
	}
	Array<Vec3> verts(m_allocator);

	i32 total_vertex_count = 0;
	for (auto& mesh : m_meshes)	{
		total_vertex_count += (i32)(mesh.vertex_data.size() / getVertexSize(*mesh.fbx, mesh.is_skinned, cfg));
	}
	verts.reserve(total_vertex_count);

	for (auto& mesh : m_meshes) {
		int vertex_size = getVertexSize(*mesh.fbx, mesh.is_skinned, cfg);
		int vertex_count = (i32)(mesh.vertex_data.size() / vertex_size);

		const u8* src = mesh.vertex_data.data();

		for (int i = 0; i < vertex_count; ++i) {
			verts.push(*(Vec3*)(src + i * vertex_size));
		}
	}

	if (to_convex) {
		if (!ps->cookConvex(verts, out_file)) {
			logError("Failed to cook ", src);
			return;
		}
	} else {
		Array<u32> indices(m_allocator);
		i32 count = 0;
		for (auto& mesh : m_meshes) {
			count += mesh.indices.size();
		}
		indices.reserve(count);
		int offset = 0;
		for (auto& mesh : m_meshes) {
			for (unsigned int j = 0, c = mesh.indices.size(); j < c; ++j) {
				u32 index = mesh.indices[j] + offset;
				indices.push(index);
			}
			int vertex_size = getVertexSize(*mesh.fbx, mesh.is_skinned, cfg);
			int vertex_count = (i32)(mesh.vertex_data.size() / vertex_size);
			offset += vertex_count;
		}

		if (!ps->cookTriMesh(verts, indices, out_file)) {
			logError("Failed to cook ", src);
			return;
		}
	}

	Path phy_path(".phy:", src);
	m_compiler.writeCompiledResource(phy_path, Span(out_file.data(), (i32)out_file.size()));
}


void FBXImporter::writePrefab(const Path& src, const ImportConfig& cfg)
{
	// TODO this is not threadsafe, since it can load/unload assets, access lua state, ...
	Engine& engine = m_app.getEngine();
	World& world = engine.createWorld(false);

	os::OutputFile file;
	PathInfo file_info(src);
	Path tmp(file_info.dir, "/", file_info.basename, ".fab");
	if (!m_filesystem.open(tmp, file)) {
		logError("Could not create ", tmp);
		return;
	}

	OutputMemoryStream blob(m_allocator);
	
	const EntityRef root = world.createEntity({0, 0, 0}, Quat::IDENTITY);

	static const ComponentType MODEL_INSTANCE_TYPE = reflection::getComponentType("model_instance");
	for(int i  = 0; i < m_meshes.size(); ++i) {
		const EntityRef e = world.createEntity(DVec3(fixOrientation(m_meshes[i].origin) * cfg.mesh_scale * m_fbx_scale), Quat::IDENTITY);
		world.createComponent(MODEL_INSTANCE_TYPE, e);
		world.setParent(root, e);
		char mesh_name[256];
		getImportMeshName(m_meshes[i], mesh_name);
		Path mesh_path(mesh_name, ".fbx:", src);
		RenderModule* m_scene = (RenderModule*)world.getModule(MODEL_INSTANCE_TYPE);
		m_scene->setModelInstancePath(e, mesh_path);
	}

	static const ComponentType POINT_LIGHT_TYPE = reflection::getComponentType("point_light");
	for (i32 i = 0, c = m_scene->getLightCount(); i < c; ++i) {
		const ofbx::Light* light = m_scene->getLight(i);
		const Matrix mtx = toLumix(light->getGlobalTransform());
		const EntityRef e = world.createEntity(DVec3(mtx.getTranslation() * cfg.mesh_scale * m_fbx_scale), Quat::IDENTITY);
		world.createComponent(POINT_LIGHT_TYPE, e);
		world.setParent(root, e);
	}

	world.serialize(blob, WorldSerializeFlags::NONE);
	engine.destroyWorld(world);

	if (!file.write(blob.data(), blob.size())) {
		logError("Could not write ", tmp);
	}
	file.close();
}


void FBXImporter::writeSubmodels(const Path& src, const ImportConfig& cfg)
{
	PROFILE_FUNCTION();
	postprocessMeshes(cfg, src);

	for (int i = 0; i < m_meshes.size(); ++i) {
		char name[256];
		getImportMeshName(m_meshes[i], name);

		out_file.clear();
		writeModelHeader();
		writeMeshes(src, i, cfg);
		writeGeometry(i, cfg);
		if (m_meshes[i].is_skinned) {
			writeSkeleton(cfg);
		}
		else {
			m_bind_pose.clear();
			write((i32)0);
		}

		// lods
		const i32 lod_count = 1;
		const i32 to_mesh = 0;
		const float factor = FLT_MAX;
		write(lod_count);
		write(to_mesh);
		write(factor);

		Path path(name, ".fbx:", src);

		m_compiler.writeCompiledResource(path, Span(out_file.data(), (i32)out_file.size()));
	}
}


void FBXImporter::writeModel(const Path& src, const ImportConfig& cfg)
{
	PROFILE_FUNCTION();
	postprocessMeshes(cfg, src);

	bool import_any_mesh = false;
	for (const ImportMesh& m : m_meshes) {
		if (m.import) import_any_mesh = true;
	}
	if (!import_any_mesh && m_animations.empty()) return;

	out_file.clear();
	writeModelHeader();
	writeMeshes(src, -1, cfg);
	writeGeometry(cfg);
	writeSkeleton(cfg);
	writeLODs(cfg);

	m_compiler.writeCompiledResource(Path(src), Span(out_file.data(), (i32)out_file.size()));
}


} // namespace Lumix