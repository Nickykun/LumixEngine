#include "animation.h"
#include "condition.h"
#include "controller.h"
#include "engine/log.h"
#include "nodes.h"
#include "renderer/model.h"
#include "renderer/pose.h"
#include "engine/crt.h"
#include "engine/stack_array.h"

namespace Lumix::anim {

static LUMIX_FORCE_INLINE LocalRigidTransform getRootMotionEx(const Animation* anim, Time t0, Time t1) {
	ASSERT(t0 <= t1);
	LocalRigidTransform old_tr = anim->getRootMotion(t0).inverted();
	LocalRigidTransform new_tr = anim->getRootMotion(t1);
	return old_tr * new_tr;
}

static LUMIX_FORCE_INLINE LocalRigidTransform getRootMotion(const RuntimeContext& ctx, const Animation* anim, Time t0_abs, Time t1_abs) {
	const Time t0 = t0_abs % anim->getLength();
	const Time t1 = t1_abs % anim->getLength();
	
	if (t0 <= t1) return getRootMotionEx(anim, t0, t1);
	
	const LocalRigidTransform tr_0 = getRootMotionEx(anim, t0, anim->getLength());
	const LocalRigidTransform tr_1 = getRootMotionEx(anim, Time(0), t1);
	
	return tr_0 * tr_1;
}

RuntimeContext::RuntimeContext(Controller& controller, IAllocator& allocator)
	: data(allocator)
	, inputs(allocator)
	, controller(controller)
	, animations(allocator)
	, events(allocator)
	, input_runtime(nullptr, 0)
{
}

static u32 getInputByteOffset(Controller& controller, u32 input_idx) {
	u32 offset = 0;
	for (u32 i = 0; i < input_idx; ++i) {
		switch (controller.m_inputs.inputs[i].type) {
			case InputDecl::FLOAT: offset += sizeof(float); break;
			case InputDecl::BOOL: offset += sizeof(bool); break;
			case InputDecl::U32: offset += sizeof(u32); break;
			case InputDecl::EMPTY: break;
		}
	}
	return offset;
}

void RuntimeContext::setInput(u32 input_idx, float value) {
	ASSERT(controller.m_inputs.inputs[input_idx].type == InputDecl::FLOAT);
	const u32 offset = getInputByteOffset(controller, input_idx);
	memcpy(&inputs[offset], &value, sizeof(value));
}

void RuntimeContext::setInput(u32 input_idx, bool value) {
	ASSERT(controller.m_inputs.inputs[input_idx].type == InputDecl::BOOL);
	const u32 offset = getInputByteOffset(controller, input_idx);
	memcpy(&inputs[offset], &value, sizeof(value));
}

static float getInputValue(const RuntimeContext& ctx, u32 idx) {
	const InputDecl::Input& input = ctx.controller.m_inputs.inputs[idx];
	ASSERT(input.type == InputDecl::FLOAT);
	return *(float*)&ctx.inputs[input.offset];
}

struct Blend2DActiveTrio {
	const Blend2DNode::Child* a;
	const Blend2DNode::Child* b;
	const Blend2DNode::Child* c;
	float ta, tb, tc;
};

bool getBarycentric(const Vec2& p, const Vec2& a, const Vec2& b, const Vec2& c, Vec2& uv) {
  const Vec2 ab = b - a, ac = c - a, ap = p - a;

  float d00 = dot(ab, ab);
  float d01 = dot(ab, ac);
  float d11 = dot(ac, ac);
  float d20 = dot(ap, ab);
  float d21 = dot(ap, ac);
  float denom = d00 * d11 - d01 * d01;

  uv.x = (d11 * d20 - d01 * d21) / denom;
  uv.y = (d00 * d21 - d01 * d20) / denom;  
  return uv.x >= 0.f && uv.y >= 0.f && uv.x + uv.y <= 1.f;
}

static Blend2DActiveTrio getActiveTrio(const Blend2DNode& node, Vec2 input_val) {
	const Blend2DNode::Child* children = node.m_children.begin();
	Vec2 uv;
	for (const Blend2DNode::Triangle& t : node.m_triangles) {
		if (!getBarycentric(input_val, children[t.a].value, children[t.b].value, children[t.c].value, uv)) continue;
		
		Blend2DActiveTrio res;
		res.a = &node.m_children[t.a];
		res.b = &node.m_children[t.b];
		res.c = &node.m_children[t.c];
		res.ta = 1 - uv.x - uv.y;
		res.tb = uv.x;
		res.tc = uv.y;
		return res;
	}

	Blend2DActiveTrio res;
	res.a = node.m_children.begin();
	res.b = node.m_children.begin();
	res.c = node.m_children.begin();
	res.ta = 1;
	res.tb = res.tc = 0;
	return res;
}

Blend2DNode::Blend2DNode(Node* parent, IAllocator& allocator)
	: Node(parent, allocator) 
	, m_children(allocator)
	, m_triangles(allocator)
{}

void Blend2DNode::update(RuntimeContext& ctx, LocalRigidTransform& root_motion) const {
	float relt = ctx.input_runtime.read<float>();
	const float relt0 = relt;
	
	if (m_children.size() > 2) {
		Vec2 input_val;
		input_val.x = getInputValue(ctx, m_x_input_index);
		input_val.y = getInputValue(ctx, m_y_input_index);
		const Blend2DActiveTrio trio = getActiveTrio(*this, input_val);
		const Animation* anim_a = ctx.animations[trio.a->slot];
		const Animation* anim_b = ctx.animations[trio.b->slot];
		const Animation* anim_c = ctx.animations[trio.c->slot];
		if (!anim_a || !anim_b || !anim_c || !anim_a->isReady() || !anim_b->isReady() || !anim_c->isReady()) {
			ctx.data.write(relt);
			return;
		}
	
		const Time wlen = anim_a->getLength() * trio.ta + anim_b->getLength() * trio.tb + anim_c->getLength() * trio.tc;
		relt += ctx.time_delta / wlen;
		relt = fmodf(relt, 1);
		
		{
			const Time len = anim_a->getLength();
			const Time t0 = len * relt0;
			const Time t = len * relt;
			root_motion = getRootMotion(ctx, anim_a, t0, t);
		}
	
		if (trio.tb > 0) {
			const Time len = anim_b->getLength();
			const Time t0 = len * relt0;
			const Time t = len * relt;
			const LocalRigidTransform tr1 = getRootMotion(ctx, anim_b, t0, t);
			root_motion = root_motion.interpolate(tr1, trio.tb / (trio.ta + trio.tb));
		}
	
		if (trio.tc > 0) {
			const Time len = anim_c->getLength();
			const Time t0 = len * relt0;
			const Time t = len * relt;
			const LocalRigidTransform tr1 = getRootMotion(ctx, anim_c, t0, t);
			root_motion = root_motion.interpolate(tr1, trio.tc);
		}
	}

	ctx.data.write(relt);
}

Time Blend2DNode::length(const RuntimeContext& ctx) const {
	if (m_children.size() < 3) return Time(1);

	Vec2 input_val;
	input_val.x = getInputValue(ctx, m_x_input_index);
	input_val.y = getInputValue(ctx, m_y_input_index);
	const Blend2DActiveTrio trio = getActiveTrio(*this, input_val);

	Animation* anim_a = ctx.animations[trio.a->slot];
	Animation* anim_b = ctx.animations[trio.b->slot];
	Animation* anim_c = ctx.animations[trio.c->slot];
	if (!anim_a || !anim_a->isReady()) return Time::fromSeconds(1);
	if (!anim_b || !anim_b->isReady()) return Time::fromSeconds(1);
	if (!anim_c || !anim_c->isReady()) return Time::fromSeconds(1);
	
	return anim_a->getLength() * trio.ta + anim_b->getLength() * trio.tb + anim_c->getLength() * trio.tc;
}

void Blend2DNode::enter(RuntimeContext& ctx) const {
	const float t = 0.f;
	ctx.data.write(t);
}

void Blend2DNode::skip(RuntimeContext& ctx) const {
	ctx.input_runtime.skip(sizeof(float));
}

static void getPose(const RuntimeContext& ctx, float rel_time, float weight, u32 slot, Pose& pose, u32 mask_idx, bool looped) {
	Animation* anim = ctx.animations[slot];
	if (!anim) return;
	if (!ctx.model->isReady()) return;
	if (!anim->isReady()) return;

	Time time = anim->getLength() * rel_time;
	const Time anim_time = looped ? time % anim->getLength() : minimum(time, anim->getLength());

	const BoneMask* mask = mask_idx < (u32)ctx.controller.m_bone_masks.size() ? &ctx.controller.m_bone_masks[mask_idx] : nullptr;

	Animation::SampleContext sample_ctx;
	sample_ctx.pose = &pose;
	sample_ctx.time = anim_time;
	sample_ctx.model = ctx.model;
	sample_ctx.weight = weight;
	sample_ctx.mask = mask;
	anim->setRootMotionBone(ctx.root_bone_hash);
	anim->getRelativePose(sample_ctx);
}

static void getPose(const RuntimeContext& ctx, Time time, float weight, u32 slot, Pose& pose, u32 mask_idx, bool looped) {
	Animation* anim = ctx.animations[slot];
	if (!anim) return;
	if (!ctx.model->isReady()) return;
	if (!anim->isReady()) return;

	const Time anim_time = looped ? time % anim->getLength() : minimum(time, anim->getLength());

	Animation::SampleContext sample_ctx;
	sample_ctx.pose = &pose;
	sample_ctx.time = anim_time;
	sample_ctx.model = ctx.model;
	sample_ctx.weight = weight;
	sample_ctx.mask = mask_idx < (u32)ctx.controller.m_bone_masks.size() ? &ctx.controller.m_bone_masks[mask_idx] : nullptr;
	anim->setRootMotionBone(ctx.root_bone_hash);
	anim->getRelativePose(sample_ctx);
}

void Blend2DNode::getPose(RuntimeContext& ctx, float weight, Pose& pose, u32 mask) const {
	const float t = ctx.input_runtime.read<float>();

	if (m_children.empty()) return;
	if (m_children.size() < 3) {
		anim::getPose(ctx, t, weight, m_children[0].slot, pose, mask, true);
		return;
	}

	Vec2 input_val;
	input_val.x = getInputValue(ctx, m_x_input_index);
	input_val.y = getInputValue(ctx, m_y_input_index);
	const Blend2DActiveTrio trio = getActiveTrio(*this, input_val);
	
	anim::getPose(ctx, t, weight, trio.a->slot, pose, mask, true);
	if (trio.tb > 0) anim::getPose(ctx, t, weight * trio.tb, trio.b->slot, pose, mask, true);
	if (trio.tc > 0) anim::getPose(ctx, t, weight * trio.tc, trio.c->slot, pose, mask, true);
}

static Vec2 computeCircumcircleCenter(Vec2 a, Vec2 b, Vec2 c) {
	Vec2 dab = b - a;
	Vec2 dac = c - a;
	Vec2 o = (dac * squaredLength(dab) - dab * squaredLength(dac)).ortho() / ((dab.x * dac.y - dab.y * dac.x) * 2.f);
	return o + a;
}

// delaunay triangulation
void Blend2DNode::dataChanged(IAllocator& allocator) {
	m_triangles.clear();
	if (m_children.size() < 3) return;

	struct Edge {
		u32 a, b;
		bool valid = true;
		bool operator ==(const Edge& rhs) {
			return a == rhs.a && b == rhs.b || a == rhs.b && b == rhs.a;
		}
	};

	StackArray<Edge, 8> edges(allocator);

	auto pushTriangle = [&](u32 a, u32 b, u32 c){
		Triangle& t = m_triangles.emplace();
		t.a = a;
		t.b = b;
		t.c = c;
		t.circumcircle_center = computeCircumcircleCenter(m_children[a].value, m_children[b].value, m_children[c].value);
	};

	Vec2 min = Vec2(FLT_MAX);
	Vec2 max = Vec2(-FLT_MAX);
	for (const Child& i : m_children) {
		min = minimum(min, i.value);
		max = maximum(max, i.value);
	}
	
	{
		// bounding triangle
		Vec2 d = max - min;
		float dmax = maximum(d.x, d.y);
		Vec2 mid = (max + min) * 0.5f;
		m_children.emplace().value = Vec2(mid.x - 20 * dmax, mid.y - dmax);
		m_children.emplace().value = Vec2(mid.x, mid.y + 20 * dmax);
		m_children.emplace().value = Vec2(mid.x + 20 * dmax, mid.y - dmax);
		pushTriangle(m_children.size() - 1, m_children.size() - 2, 0);
		pushTriangle(m_children.size() - 2, m_children.size() - 3, 0);
		pushTriangle(m_children.size() - 3, m_children.size() - 1, 0);
	}

	for (u32 ch = 1, c = m_children.size() - 3; ch < c; ++ch) {
		Vec2 p = m_children[ch].value;
		edges.clear();

		for (i32 ti = m_triangles.size() - 1; ti >= 0; --ti) {
			const Triangle& t = m_triangles[ti];
			Vec2 center = t.circumcircle_center;
			if (squaredLength(p - center) > squaredLength(m_children[t.a].value - center)) continue;

			edges.push({t.a, t.b});
			edges.push({t.b, t.c});
			edges.push({t.c, t.a});

			m_triangles.swapAndPop(ti);
		}

		for (i32 i = edges.size() - 1; i > 0; --i) {
			for (i32 j = i - 1; j >= 0; --j) {
				if (edges[i] == edges[j]) {
					edges[i].valid = false;
					edges[j].valid = false;
				}
			}
		}

		edges.eraseItems([](const Edge& e){ return !e.valid; });

		for (Edge& e : edges) {
			pushTriangle(e.a, e.b, ch);
		}
	}

	// pop bounding triangle's vertices and remove related triangles
	m_children.pop();
	m_children.pop();
	m_children.pop();

	m_triangles.eraseItems([&](const Triangle& t){
		const u32 s = (u32)m_children.size();
		return t.a >= s || t.b >= s || t.c >= s;
	});
}

Time Blend2DNode::time(const RuntimeContext& ctx) const {
	return length(ctx) * ctx.input_runtime.getAs<float>();
}

void Blend2DNode::serialize(OutputMemoryStream& stream) const {
	Node::serialize(stream);
	stream.write(m_x_input_index);
	stream.write(m_y_input_index);
	stream.writeArray(m_children);
}

void Blend2DNode::deserialize(InputMemoryStream& stream, Controller& ctrl, u32 version) {
	Node::deserialize(stream, ctrl, version);
	stream.read(m_x_input_index);
	stream.read(m_y_input_index);
	stream.readArray(&m_children);
	dataChanged(ctrl.m_allocator);
}

Blend1DNode::Blend1DNode(Node* parent, IAllocator& allocator)
	: Node(parent, allocator) 
	, m_children(allocator)
{}

struct Blend1DActivePair {
	const Blend1DNode::Child* a;
	const Blend1DNode::Child* b;
	float t;
};

static Blend1DActivePair getActivePair(const Blend1DNode& node, float input_val) {
	const auto& children = node.m_children;
	if (input_val > children[0].value) {
		if (input_val >= children.back().value) {
			return { &children.back(), nullptr, 0 };
		}
		else {
			for (u32 i = 1, c = children.size(); i < c; ++i) {
				if (input_val < children[i].value) {
					const float w = (input_val - children[i - 1].value) / (children[i].value - children[i - 1].value);
					return { &children[i - 1], &children[i], w };
				}
			}
		}
	}
	return { &children[0], nullptr, 0 };
}

void Blend1DNode::update(RuntimeContext& ctx, LocalRigidTransform& root_motion) const {
	float relt = ctx.input_runtime.read<float>();
	const float relt0 = relt;
	
	const float input_val = getInputValue(ctx, m_input_index);
	const Blend1DActivePair pair = getActivePair(*this, input_val);
	const Animation* anim_a = pair.a ? ctx.animations[pair.a->slot] : nullptr;
	const Animation* anim_b = pair.b ? ctx.animations[pair.b->slot] : nullptr;
	const Time wlen = anim_a ? lerp(anim_a->getLength(), anim_b ? anim_b->getLength() : anim_a->getLength(), pair.t) : Time::fromSeconds(1);
	relt += ctx.time_delta / wlen;
	relt = fmodf(relt, 1);
	
	if (anim_a) {
		const Time len = anim_a->getLength();
		const Time t0 = len * relt0;
		const Time t = len * relt;
		root_motion = getRootMotion(ctx, ctx.animations[pair.a->slot], t0, t);
	}
	else {
		root_motion = {{0, 0, 0}, {0, 0, 0, 1}};
	}
	if (anim_b && anim_b->isReady()) {
		const Time len = anim_b->getLength();
		const Time t0 = len * relt0;
		const Time t = len * relt;
		const LocalRigidTransform tr1 = getRootMotion(ctx, ctx.animations[pair.b->slot], t0, t);
		root_motion = root_motion.interpolate(tr1, pair.t);
	}

	ctx.data.write(relt);
}

Time Blend1DNode::length(const RuntimeContext& ctx) const {
	const float input_val = getInputValue(ctx, m_input_index);
	const Blend1DActivePair pair = getActivePair(*this, input_val);
	Animation* anim_a = ctx.animations[pair.a->slot];
	if (!anim_a) return Time::fromSeconds(1);
	if (!anim_a->isReady()) return Time::fromSeconds(1);
	
	Animation* anim_b = pair.b ? ctx.animations[pair.b->slot] : nullptr;
	if (!anim_b) return anim_a->getLength();
	if (!anim_b->isReady()) return anim_a->getLength();

	return lerp(anim_a->getLength(), anim_b->getLength(), pair.t);
}

Time Blend1DNode::time(const RuntimeContext& ctx) const {
	return length(ctx) * ctx.input_runtime.getAs<float>();
}

void Blend1DNode::enter(RuntimeContext& ctx) const {
	const float t = 0.f;
	ctx.data.write(t);
}

void Blend1DNode::skip(RuntimeContext& ctx) const {
	ctx.input_runtime.skip(sizeof(float));
}

void Blend1DNode::getPose(RuntimeContext& ctx, float weight, Pose& pose, u32 mask) const {
	const float t = ctx.input_runtime.read<float>();

	if (m_children.empty()) return;
	if (m_children.size() == 1) {
		anim::getPose(ctx, t, weight, m_children[0].slot, pose, mask, true);
		return;
	}

	const float input_val = getInputValue(ctx, m_input_index);
	const Blend1DActivePair pair = getActivePair(*this, input_val);
	
	anim::getPose(ctx, t, weight, pair.a->slot, pose, mask, true);
	if (pair.b) {
		anim::getPose(ctx, t, weight * pair.t, pair.b->slot, pose, mask, true);
	}
}

void Blend1DNode::serialize(OutputMemoryStream& stream) const {
	Node::serialize(stream);
	stream.write(m_input_index);
	stream.write((u32)m_children.size());
	stream.write(m_children.begin(), m_children.byte_size());
}

void Blend1DNode::deserialize(InputMemoryStream& stream, Controller& ctrl, u32 version) {
	Node::deserialize(stream, ctrl, version);
	stream.read(m_input_index);
	u32 count;
	stream.read(count);
	m_children.resize(count);
	stream.read(m_children.begin(), m_children.byte_size());
}

ConditionNode::ConditionNode(Node* parent, IAllocator& allocator)
	: Node(parent, allocator)
	, m_condition(allocator)
	, m_condition_str(allocator)
	, m_allocator(allocator)
{
}

ConditionNode::~ConditionNode() {
	LUMIX_DELETE(m_allocator, m_true_node);
	LUMIX_DELETE(m_allocator, m_false_node);
}

void ConditionNode::update(RuntimeContext& ctx, LocalRigidTransform& root_motion) const {
	if (!m_true_node || !m_false_node) return;

	RuntimeData data = ctx.input_runtime.read<RuntimeData>();
	
	const bool is_transitioning = data.t < m_blend_length;
	if (is_transitioning) {
		data.t += ctx.time_delta;
		
		const bool transition_finished = data.t >= m_blend_length;
		if (transition_finished) {
			// TODO remaining root motion from skipped node
			(data.is_true ? m_false_node : m_true_node)->skip(ctx);
			ctx.data.write(data);
			(data.is_true ? m_true_node : m_false_node)->update(ctx, root_motion);
			return;
		}
		
		ctx.data.write(data);

		(data.is_true ? m_false_node : m_true_node)->update(ctx, root_motion);
		LocalRigidTransform tmp;
		(data.is_true ? m_true_node : m_false_node)->update(ctx, tmp);
		root_motion = root_motion.interpolate(tmp, data.t.seconds() / m_blend_length.seconds());
		return;
	}
	
	const bool is_true = m_condition.eval(ctx);
	if (data.is_true != is_true) {
		if (m_blend_length.raw() == 0) {
			(data.is_true ? m_true_node : m_false_node)->skip(ctx);
			data.is_true = is_true;
			ctx.data.write(data);
			(data.is_true ? m_true_node : m_false_node)->enter(ctx);
			//(data.is_true ? m_true_node : m_false_node)->update(ctx, root_motion);
		}
		else {
			data.t = Time(0);
			data.is_true = is_true;
			ctx.data.write(data);
			(is_true ? m_false_node : m_true_node)->update(ctx, root_motion);
			(is_true ? m_true_node : m_false_node)->enter(ctx);
		}
		return;
	}

	ctx.data.write(data);
	(data.is_true ? m_true_node : m_false_node)->update(ctx, root_motion);
}

void ConditionNode::enter(RuntimeContext& ctx) const {
	if (!m_true_node || !m_false_node) return;

	RuntimeData rdata;
	rdata.t = m_blend_length;
	rdata.is_true = m_condition.eval(ctx);
	ctx.data.write(rdata);
	(rdata.is_true ? m_true_node : m_false_node)->enter(ctx);
}

void ConditionNode::skip(RuntimeContext& ctx) const {
	if (!m_true_node || !m_false_node) return;

	RuntimeData data = ctx.input_runtime.read<RuntimeData>();
	Node* running_child = data.is_true ? m_true_node : m_false_node;
	running_child->skip(ctx);
}

void ConditionNode::getPose(RuntimeContext& ctx, float weight, Pose& pose, u32 mask) const {
	if (!m_true_node || !m_false_node) return;

	const RuntimeData data = ctx.input_runtime.read<RuntimeData>();
	const bool is_transitioning = data.t < m_blend_length;
	if (is_transitioning) {
		(data.is_true ? m_false_node : m_true_node)->getPose(ctx, weight, pose, mask);
		const float t = clamp(data.t / m_blend_length, 0.f, 1.f);
		(data.is_true ? m_true_node : m_false_node)->getPose(ctx, weight * t, pose, mask);
	}
	else {
		(data.is_true ? m_true_node : m_false_node)->getPose(ctx, weight, pose, mask);
	}
}

void ConditionNode::serialize(OutputMemoryStream& stream) const {
	Node::serialize(stream);

	stream.write(m_condition_str);
	stream.write(m_blend_length);

	stream.write(m_true_node != nullptr);
	if (m_true_node) {
		stream.write(m_true_node->type());
		m_true_node->serialize(stream);
	}
	stream.write(m_false_node != nullptr);
	if (m_false_node) {
		stream.write(m_false_node->type());
		m_false_node->serialize(stream);
	}
}

void ConditionNode::deserialize(InputMemoryStream& stream, Controller& ctrl, u32 version) {
	Node::deserialize(stream, ctrl, version);

	stream.read(m_condition_str);
	m_condition.compile(m_condition_str.c_str(), ctrl.m_inputs);
	stream.read(m_blend_length);

	if (stream.read<bool>()) {
		Node::Type type;
		stream.read(type);
		m_true_node = Node::create(this, type, m_allocator);
		m_true_node->deserialize(stream, ctrl, version);
	}

	if (stream.read<bool>()) {
		Node::Type type;
		stream.read(type);
		m_false_node = Node::create(this, type, m_allocator);
		m_false_node->deserialize(stream, ctrl, version);
	}
}

Time ConditionNode::length(const RuntimeContext& ctx) const {
	return Time::fromSeconds(1);
}

Time ConditionNode::time(const RuntimeContext& ctx) const {
	return Time::fromSeconds(0);
}

AnimationNode::AnimationNode(Node* parent, IAllocator& allocator) 
	: Node(parent, allocator) 
{}

void Node::emitEvents(Time old_time, Time new_time, Time loop_length, RuntimeContext& ctx) const {
	// TODO add emitEvents to all nodes (where applicable)
	if (m_events.empty()) return;

	InputMemoryStream blob(m_events);
	const Time t0 = old_time % loop_length;
	const Time t1 = new_time % loop_length;

	const u16 from = u16(0xffFF * (u64)t0.raw() / loop_length.raw());
	const u16 to = u16(0xffFF * (u64)t1.raw() / loop_length.raw());

	if (t1.raw() >= t0.raw()) {
		while(blob.getPosition() < blob.size()) {
			const u32 type = blob.read<u32>();
			const u16 size = blob.read<u16>();
			const u16 rel_time = blob.read<u16>();
			if (rel_time >= from && rel_time < to) {
				ctx.events.write((u8*)blob.getData() + blob.getPosition() - 2 * sizeof(u32), size + 2 * sizeof(u32));
			}
			blob.skip(size);
		}
	}
	else {
		emitEvents(t0, loop_length, Time::fromSeconds(loop_length.seconds() + 1), ctx);
		emitEvents(Time(0), t1, loop_length, ctx);
	}
}

void AnimationNode::update(RuntimeContext& ctx, LocalRigidTransform& root_motion) const {
	Time t = ctx.input_runtime.read<Time>();
	Time prev_t = t;
	t += ctx.time_delta;

	Animation* anim = ctx.animations[m_slot];
	if (anim && anim->isReady()) {
		if ((m_flags & LOOPED) == 0) {
			const u32 len = anim->getLength().raw();
			t = Time(minimum(t.raw(), len));
			prev_t = Time(minimum(prev_t.raw(), len));
		}

		emitEvents(prev_t, t, anim->getLength(), ctx);
		root_motion = getRootMotion(ctx, anim, prev_t, t);
	}
	else {
		root_motion = {{0, 0, 0}, {0, 0, 0, 1}};
	}
	ctx.data.write(t);
}

Time AnimationNode::length(const RuntimeContext& ctx) const {
	Animation* anim = ctx.animations[m_slot];
	if (!anim) return Time(0);
	return anim->getLength();
}

Time AnimationNode::time(const RuntimeContext& ctx) const {
	return ctx.input_runtime.getAs<Time>();
}

void AnimationNode::enter(RuntimeContext& ctx) const {
	Time t = Time(0); 
	ctx.data.write(t);	
}

void AnimationNode::skip(RuntimeContext& ctx) const {
	ctx.input_runtime.skip(sizeof(Time));
}
	
void AnimationNode::getPose(RuntimeContext& ctx, float weight, Pose& pose, u32 mask) const {
	const Time t = ctx.input_runtime.read<Time>();
	anim::getPose(ctx, t, weight, m_slot, pose, mask, m_flags & LOOPED);
}

void AnimationNode::serialize(OutputMemoryStream& stream) const {
	Node::serialize(stream);
	stream.write(m_slot);
	stream.write(m_flags);
}

void AnimationNode::deserialize(InputMemoryStream& stream, Controller& ctrl, u32 version) {
	Node::deserialize(stream, ctrl, version);
	stream.read(m_slot);
	stream.read(m_flags);
}

LayersNode::Layer::Layer(IAllocator& allocator) 
	: name(allocator)
{
}

LayersNode::LayersNode(Node* parent, IAllocator& allocator) 
	: Node(parent, allocator)
	, m_layers(allocator)
	, m_allocator(allocator)
{
}

LayersNode::~LayersNode() {
	for (Layer& l : m_layers) {
		LUMIX_DELETE(m_allocator, l.node);
	}
}

void LayersNode::update(RuntimeContext& ctx, LocalRigidTransform& root_motion) const {
	for (const Layer& layer : m_layers) {
		LocalRigidTransform tmp_rm;
		layer.node->update(ctx, tmp_rm);
		if (&layer == m_layers.begin()) {
			root_motion = tmp_rm;
		}
	}
}

Time LayersNode::length(const RuntimeContext& ctx) const {
	return Time::fromSeconds(1);
}

Time LayersNode::time(const RuntimeContext& ctx) const {
	return Time(0);
}

void LayersNode::enter(RuntimeContext& ctx) const {
	for (const Layer& layer : m_layers) {
		layer.node->enter(ctx);
	}
}

void LayersNode::skip(RuntimeContext& ctx) const {
	for (const Layer& layer : m_layers) {
		layer.node->skip(ctx);
	}
}

void LayersNode::getPose(RuntimeContext& ctx, float weight, Pose& pose, u32 mask) const {
	for (const Layer& layer : m_layers) {
		layer.node->getPose(ctx, weight, pose, layer.mask);
	}
}

void LayersNode::serialize(OutputMemoryStream& stream) const {
	stream.write((u32)m_layers.size());
	for (const Layer& layer : m_layers) {
		stream.writeString(layer.name);
		stream.write(layer.mask);
		stream.write(layer.node->type());
		layer.node->serialize(stream);
	}
}

void LayersNode::deserialize(InputMemoryStream& stream, Controller& ctrl, u32 version) {
	Node::deserialize(stream, ctrl, version);
	u32 c;
	stream.read(c);
	for (u32 i = 0; i < c; ++i) {
		Layer& layer = m_layers.emplace(m_allocator);
		layer.name = stream.readString();
		stream.read(layer.mask);
		Node::Type type;
		stream.read(type);
		layer.node = Node::create(this, type, m_allocator);
		layer.node->deserialize(stream, ctrl, version);
	}
}

SelectNode::SelectNode(Node* parent, IAllocator& allocator)
	: Node(parent, allocator)
	, m_allocator(allocator)
	, m_children(allocator)
{}

SelectNode::~SelectNode() {
	for (Child& c : m_children) {
		LUMIX_DELETE(m_allocator, c.node);
	}
}

u32 SelectNode::getChildIndex(float input_val) const {
	ASSERT(m_children.size() > 0);
	for (u32 i = 0, c = m_children.size(); i < c; ++i) {
		const Child& child = m_children[i];
		if (input_val <= child.max_value) {
			return i;
		}
	}
	return m_children.size() - 1;
}

void SelectNode::update(RuntimeContext& ctx, LocalRigidTransform& root_motion) const {
	if (m_children.empty()) return;

	RuntimeData data = ctx.input_runtime.read<RuntimeData>();
	
	const float input_val = getInputValue(ctx, m_input_index);
	const u32 child_idx = getChildIndex(input_val);

	if (data.from != data.to) {
		data.t += ctx.time_delta;

		if (m_blend_length < data.t) {
			// TODO root motion in data.from
			m_children[data.from].node->skip(ctx);
			data.from = data.to;
			data.t = Time(0);
			ctx.data.write(data);
			m_children[data.to].node->update(ctx, root_motion);
			return;
		}

		ctx.data.write(data);

		m_children[data.from].node->update(ctx, root_motion);
		LocalRigidTransform tmp;
		m_children[data.to].node->update(ctx, tmp);
		root_motion = root_motion.interpolate(tmp, data.t.seconds() / m_blend_length.seconds());
		return;
	}

	if (child_idx != data.from) {
		data.to = child_idx;
		data.t = Time(0);
		ctx.data.write(data);
		m_children[data.from].node->update(ctx, root_motion);
		m_children[data.to].node->enter(ctx);
		return;
	}

	data.t += ctx.time_delta;
	ctx.data.write(data);
	m_children[data.from].node->update(ctx, root_motion);
}

void SelectNode::enter(RuntimeContext& ctx) const {
	if (m_children.empty()) return;

	RuntimeData runtime_data = { 0, 0, Time(0) };
	const float input_val = getInputValue(ctx, m_input_index);
	runtime_data.from = getChildIndex(input_val);
	runtime_data.to = runtime_data.from;
	ctx.data.write(runtime_data);
	if (runtime_data.from < (u32)m_children.size()) {
		m_children[runtime_data.from].node->enter(ctx);
	}
}

void SelectNode::skip(RuntimeContext& ctx) const {
	if (m_children.empty()) return;

	RuntimeData data = ctx.input_runtime.read<RuntimeData>();
	m_children[data.from].node->skip(ctx);
	if (data.from != data.to) {
		m_children[data.to].node->skip(ctx);
	}
}

void SelectNode::getPose(RuntimeContext& ctx, float weight, Pose& pose, u32 mask) const {
	if (m_children.empty()) return;

	const RuntimeData data = ctx.input_runtime.read<RuntimeData>();

	m_children[data.from].node->getPose(ctx, weight, pose, mask);
	if(data.from != data.to) {
		const float t = clamp(data.t.seconds() / m_blend_length.seconds(), 0.f, 1.f);
		m_children[data.to].node->getPose(ctx, weight * t, pose, mask);
	}
}

Time SelectNode::length(const RuntimeContext& ctx) const {	return Time::fromSeconds(1); }

Time SelectNode::time(const RuntimeContext& ctx) const { return Time(0); }

void SelectNode::deserialize(InputMemoryStream& stream, Controller& ctrl, u32 version) {
	Node::deserialize(stream, ctrl, version);
	stream.read(m_blend_length);
	stream.read(m_input_index);
	u32 size;
	stream.read(size);
	m_children.reserve(size);
	for (u32 i = 0; i < size; ++i) {
		Child& child = m_children.emplace();
		stream.read(child.max_value);
		Node::Type type;
		stream.read(type);
		child.node = Node::create(this, type, m_allocator);
		child.node->deserialize(stream, ctrl, version);
	}
}

void SelectNode::serialize(OutputMemoryStream& stream) const {
	Node::serialize(stream);
	stream.write(m_blend_length);
	stream.write(m_input_index);
	stream.write((u32)m_children.size());
	for (const Child& child : m_children) {
		stream.write(child.max_value);
		stream.write(child.node->type());
		child.node->serialize(stream);
	}
}

GroupNode::GroupNode(Node* parent, IAllocator& allocator)
	: Node(parent, allocator)
	, m_allocator(allocator)
	, m_children(allocator)
	, m_transitions(allocator)
{}

GroupNode::~GroupNode() {
	for (Child& c : m_children) {
		LUMIX_DELETE(m_allocator, c.node);
	}
}

void GroupNode::update(RuntimeContext& ctx, LocalRigidTransform& root_motion) const {
	RuntimeData data = ctx.input_runtime.read<RuntimeData>();
	if (m_children.empty()) {
		ctx.data.write(data);
		return;
	}
	
	if (data.from != data.to) {
		data.t += ctx.time_delta;

		if (data.blend_length < data.t) {
			// TODO root motion in data.from
			m_children[data.from].node->skip(ctx);
			data.from = data.to;
			data.t = Time(0);
			ctx.data.write(data);
			m_children[data.to].node->update(ctx, root_motion);
			return;
		}

		ctx.data.write(data);

		m_children[data.from].node->update(ctx, root_motion);
		LocalRigidTransform tmp;
		m_children[data.to].node->update(ctx, tmp);
		root_motion = root_motion.interpolate(tmp, data.t.seconds() / data.blend_length.seconds());
		return;
	}

	const bool is_current_matching = m_children[data.from].condition.eval(ctx);
	const bool is_selectable = m_children[data.from].flags & Child::SELECTABLE;

	if (!is_current_matching || !is_selectable) {
		bool waiting_for_exit_time = false;
		bool can_go_anywhere = false;
		for (const Transition& transition : m_transitions) {
			if (transition.to == data.to) continue;
			if (transition.from != data.from && transition.from != 0xffFFffFF) continue;
			if (transition.to != 0xffFFffFF && !m_children[transition.to].condition.eval(ctx)) continue;
			
			if (transition.exit_time >= 0) {
				waiting_for_exit_time = true;
				const Time len = m_children[data.from].node->length(ctx);
				const Time beg = m_children[data.from].node->time(ctx);
				const Time end = beg + ctx.time_delta;
				const Time loop_start = beg - beg % len;
				const Time t = loop_start + Time::fromSeconds(transition.exit_time * len.seconds());
				if (t < beg || t >= end) continue;
			}

			if (transition.to == 0xffFFffFF) {
				waiting_for_exit_time = false;
				can_go_anywhere = true;
				break;
			}

			data.to = transition.to;
			data.blend_length = transition.blend_length;
			data.t = Time(0);
			ctx.data.write(data);
			m_children[data.from].node->update(ctx, root_motion);
			m_children[data.to].node->enter(ctx);
			return;
		}
		
		if ((!is_current_matching || can_go_anywhere) && !waiting_for_exit_time) {
			for (u32 i = 0, c = m_children.size(); i < c; ++i) {
				const Child& child = m_children[i];
				if (i == data.from) continue;
				if ((child.flags & Child::SELECTABLE) == 0) continue;
				if (!child.condition.eval(ctx)) continue;

				data.to = i;
				data.blend_length = m_blend_length;
				data.t = Time(0);
				ctx.data.write(data);
				m_children[data.from].node->update(ctx, root_motion);
				m_children[data.to].node->enter(ctx);
				return;
			}
		}
	}

	data.t += ctx.time_delta;
	ctx.data.write(data);
	m_children[data.from].node->update(ctx, root_motion);
}
	
Time GroupNode::length(const RuntimeContext& ctx) const {
	return Time::fromSeconds(1);
}

Time GroupNode::time(const RuntimeContext& ctx) const {
	return Time(0);
}

void GroupNode::enter(RuntimeContext& ctx) const {
	RuntimeData runtime_data = { 0, 0, Time(0) };
	for (u32 i = 0, c = m_children.size(); i < c; ++i) {
		const Child& child = m_children[i];
		
		if ((child.flags & Child::SELECTABLE) && child.condition.eval(ctx)) {
			runtime_data =  { i, i, Time(0) };
			break;
		}
	}
	ctx.data.write(runtime_data);
	if(runtime_data.from < (u32)m_children.size())
		m_children[runtime_data.from].node->enter(ctx);
}

void GroupNode::skip(RuntimeContext& ctx) const { 
	RuntimeData data = ctx.input_runtime.read<RuntimeData>();
	m_children[data.from].node->skip(ctx);
	if (data.from != data.to) {
		m_children[data.to].node->skip(ctx);
	}
}
	
void GroupNode::getPose(RuntimeContext& ctx, float weight, Pose& pose, u32 mask) const {
	const RuntimeData data = ctx.input_runtime.read<RuntimeData>();
	if (m_children.empty()) return;

	m_children[data.from].node->getPose(ctx, weight, pose, mask);
	if(data.from != data.to) {
		const float t = clamp(data.t.seconds() / data.blend_length.seconds(), 0.f, 1.f);
		m_children[data.to].node->getPose(ctx, weight * t, pose, mask);
	}
}

void GroupNode::serialize(OutputMemoryStream& stream) const {
	Node::serialize(stream);
	stream.write(m_blend_length);
	stream.write((u32)m_children.size());
	for (const Child& child : m_children) {
		stream.write(child.node->type());
		stream.write(child.flags);
		stream.writeString(child.condition_str);
		child.node->serialize(stream);
	}
	
	stream.write((u32)m_transitions.size());
	stream.write(m_transitions.begin(), m_transitions.byte_size());
}

void GroupNode::deserialize(InputMemoryStream& stream, Controller& ctrl, u32 version) {
	Node::deserialize(stream, ctrl, version);
	stream.read(m_blend_length);
	u32 size;
	stream.read(size);
	m_children.reserve(size);
	for (u32 i = 0; i < size; ++i) {
		Node::Type type;
		stream.read(type);
		m_children.emplace(m_allocator);
		if (version > (u32)ControllerVersion::TRANSITIONS) {
			stream.read(m_children[i].flags);
		}
		const char* tmp = stream.readString();
		m_children[i].condition_str = tmp;
		m_children[i].condition.compile(tmp, ctrl.m_inputs);
		m_children[i].node = Node::create(this, type, m_allocator);
		m_children[i].node->deserialize(stream, ctrl, version);
	}

	if (version > (u32)ControllerVersion::TRANSITIONS) {
		stream.read(size);
		m_transitions.resize(size);
		stream.read(m_transitions.begin(), m_transitions.byte_size());
	}
}

void Node::serialize(OutputMemoryStream& stream) const {
	stream.writeString(m_name);
	stream.write((u32)m_events.size());
	stream.write(m_events.data(), m_events.size());
}

void Node::deserialize(InputMemoryStream& stream, Controller& ctrl, u32 version) {
	m_name = stream.readString();
	if (version > (u32)ControllerVersion::EVENTS) {
		const u32 size = stream.read<u32>();
		m_events.resize(size);
		stream.read(m_events.getMutableData(), size);
	}
}

Node* Node::create(Node* parent, Type type, IAllocator& allocator) {
	switch (type) {
		case Node::ANIMATION: return LUMIX_NEW(allocator, AnimationNode)(parent, allocator);
		case Node::GROUP: return LUMIX_NEW(allocator, GroupNode)(parent, allocator);
		case Node::BLEND1D: return LUMIX_NEW(allocator, Blend1DNode)(parent, allocator);
		case Node::BLEND2D: return LUMIX_NEW(allocator, Blend2DNode)(parent, allocator);
		case Node::LAYERS: return LUMIX_NEW(allocator, LayersNode)(parent, allocator);
		case Node::CONDITION: return LUMIX_NEW(allocator, ConditionNode)(parent, allocator);
		case Node::SELECT: return LUMIX_NEW(allocator, SelectNode)(parent, allocator);
		case Node::NONE: ASSERT(false); return nullptr;
	}
	ASSERT(false);
	return nullptr;
}


} // namespace Lumix::anim
