#pragma once
#include "engine/array.h"
#include "engine/hash.h"
#include "engine/hash_map.h"
#include "engine/resource.h"
#include "engine/string.h"
#include "gpu/gpu.h"


struct lua_State;


namespace Lumix
{


struct Encoder;
struct Renderer;
struct Texture;


struct LUMIX_RENDERER_API Shader final : Resource
{
public:
	struct TextureSlot
	{
		TextureSlot()
		{
			name[0] = '\0';
			define_idx = -1;
			default_texture = nullptr;
		}

		char name[32];
		int define_idx = -1;
		Texture* default_texture = nullptr;
	};

	struct Uniform
	{
		enum Type
		{
			INT,
			FLOAT,
			MATRIX4,
			COLOR,
			VEC2,
			VEC3,
			VEC4
		};

		union
		{
			float float_value;
			float vec4[4];
			float vec3[3];
			float vec2[2];
			float matrix[16];
		} default_value;

		char name[32];
		RuntimeHash name_hash;
		Type type;
		u32 offset;
		u32 size() const;
	};

	struct Stage {
		Stage(IAllocator& allocator) : code(allocator) {}
		Stage(const Stage& rhs)
			: type(rhs.type)
			, code(rhs.code.makeCopy())
		{}

		gpu::ShaderType type;
		Array<char> code;
	};

	struct Sources {
		Sources(IAllocator& allocator) 
			: stages(allocator)
			, common(allocator)
		{}
		Sources(const Sources& rhs)
			: stages(rhs.stages.makeCopy())
			, common(rhs.common)
			, path(rhs.path)
		{}

		Path path;
		Array<Shader::Stage> stages;
		String common;
	};

	Shader(const Path& path, ResourceManager& resource_manager, Renderer& renderer, IAllocator& allocator);
	~Shader();

	ResourceType getType() const override { return TYPE; }
	bool hasDefine(u8 define) const;
	
	gpu::ProgramHandle getProgram(u32 defines);
	gpu::ProgramHandle getProgram(gpu::StateFlags state, const gpu::VertexDecl& decl, u32 defines);
	void compile(gpu::ProgramHandle program, gpu::StateFlags state, gpu::VertexDecl decl, u32 defines, Encoder& encoder);
	static void toUniformVarName(Span<char> out, const char* in);
	static void toTextureVarName(Span<char> out, const char* in);

	struct ShaderKey {
		bool operator==(const ShaderKey& rhs) const;
		gpu::StateFlags state;
		u32 defines;
		u32 decl_hash;
	};

	IAllocator& m_allocator;
	Renderer& m_renderer;
	u32 m_all_defines_mask;
	TextureSlot m_texture_slots[16];
	u32 m_texture_slot_count;
	Array<Uniform> m_uniforms;
	Array<u8> m_defines;
	HashMap<ShaderKey, gpu::ProgramHandle> m_programs;
	Sources m_sources;


	static const ResourceType TYPE;

private:
	void unload() override;
	bool load(u64 size, const u8* mem) override;
	void onBeforeReady() override;
};

template<>
struct HashFunc<Shader::ShaderKey> {
	static u32 mix(u32 a, u32 b) {
		b *= 0x5bd1e995;
		b ^= b >> 24;
		a *= 0x5bd1e995;
		return a * 0x5bd1e995;
	}

	static u32 get(const Shader::ShaderKey& key) {
		return mix(HashFunc<u64>::get(((u64*)&key)[0]), HashFunc<u64>::get(((u64*)&key)[1]));
	}
};

} // namespace Lumix
