#include "shader_editor.h"
#include "core/blob.h"
#include "core/crc32.h"
#include "core/log.h"
#include "core/math_utils.h"
#include "core/path_utils.h"
#include "core/string.h"
#include "core/system.h"
#include "utils.h"
#include <cstdio>


enum class NodeType
{
	VERTEX_INPUT,
	VERTEX_OUTPUT,
	POSITION_OUTPUT,

	FRAGMENT_INPUT,
	FRAGMENT_OUTPUT,

	FLOAT_CONST,
	COLOR_CONST,
	SAMPLE,
	MIX,
	UNIFORM,
	VEC4_MERGE,
	MULTIPLY,
	BUILTIN_UNIFORM
};


static const struct { const char* name; NodeType type; bool is_frag; bool is_vert; } NODE_TYPES[] = {
	{"Mix",								NodeType::MIX,							 true,		true},
	{"Sample",						NodeType::SAMPLE,						 true,		true},
	{"Input",							NodeType::VERTEX_INPUT,			 false,		true},
	{"Output",						NodeType::VERTEX_OUTPUT,		 false,		true},
	{"Input",							NodeType::FRAGMENT_INPUT,		 true,		false},
	{"Output",						NodeType::FRAGMENT_OUTPUT,	 true,		false},
	{"Color constant",		NodeType::COLOR_CONST,			 true,		true},
	{"Float Const",				NodeType::FLOAT_CONST,			 true,		true},
	{"Uniform",						NodeType::UNIFORM,					 true,		true},
	{"Vec4 merge",				NodeType::VEC4_MERGE,				 true,		true},
	{"Multiply",					NodeType::MULTIPLY,					 true,		true},
	{"Builtin uniforms",	NodeType::BUILTIN_UNIFORM,	 true,		true}
};


static const struct {
	ShaderEditor::VertexInput input;
	const char* gui_name;
	const char* system_name;
	ShaderEditor::ValueType type; }
VERTEX_INPUTS[] = {
	{ ShaderEditor::VertexInput::POSITION,				"Position",					"a_position",		ShaderEditor::ValueType::VEC4},
	{ ShaderEditor::VertexInput::NORMAL,					"Normal",						"a_normal",			ShaderEditor::ValueType::VEC3},
	{ ShaderEditor::VertexInput::COLOR,						"Color",						"a_color",			ShaderEditor::ValueType::VEC4},
	{ ShaderEditor::VertexInput::TANGENT,					"Tangent",					"a_tangent",		ShaderEditor::ValueType::VEC3},
	{ ShaderEditor::VertexInput::TEXCOORD0,				"Texture coord 0",	"a_texcoord0",	ShaderEditor::ValueType::VEC4},
	{ ShaderEditor::VertexInput::INSTANCE_DATA0,	"Instance data 0",	"i_data0",			ShaderEditor::ValueType::VEC4},
	{ ShaderEditor::VertexInput::INSTANCE_DATA1,	"Instance data 1",	"i_data1",			ShaderEditor::ValueType::VEC4},
	{ ShaderEditor::VertexInput::INSTANCE_DATA2,	"Instance data 2",	"i_data2",			ShaderEditor::ValueType::VEC4},
	{ ShaderEditor::VertexInput::INSTANCE_DATA3,	"Instance data 3",	"i_data3",			ShaderEditor::ValueType::VEC4}
};


enum class BuiltinUniform
{
	MODEL_MTX,
	VIEWPROJECTION_MTX
};


static const struct { BuiltinUniform uniform; const char* bgfx_name; ShaderEditor::ValueType type; } BUILTIN_UNIFORMS[] =
{
	{ BuiltinUniform::MODEL_MTX,					"u_model[0]", ShaderEditor::ValueType::MATRIX4 },
	{ BuiltinUniform::VIEWPROJECTION_MTX,	"u_viewProj", ShaderEditor::ValueType::MATRIX4 }
};


static const char* getValueTypeName(ShaderEditor::ValueType type)
{
	switch(type)
	{
		case ShaderEditor::ValueType::FLOAT: return "float";
		case ShaderEditor::ValueType::VEC2: return "vec2";
		case ShaderEditor::ValueType::VEC3: return "vec3";
		case ShaderEditor::ValueType::VEC4: return "vec4";
		case ShaderEditor::ValueType::MATRIX3: return "mat3";
		case ShaderEditor::ValueType::MATRIX4: return "mat4";
		default:
			ASSERT(false);
			return "float";
	}
}


static const char* getVertexInputBGFXName(ShaderEditor::VertexInput input)
{
	for(auto& tmp : VERTEX_INPUTS)
	{
		if(tmp.input == input) return tmp.system_name;
	}

	ASSERT(false);
	return "Error";
}


static const char* getVertexInputName(ShaderEditor::VertexInput input)
{
	for(auto& tmp : VERTEX_INPUTS)
	{
		if(tmp.input == input) return tmp.gui_name;
	}

	ASSERT(false);
	return "Error";
}


static void writeVertexShaderHeader(Lumix::OutputBlob& blob,
	const bool(&inputs)[(int)ShaderEditor::VertexInput::COUNT],
	const char(&outputs)[ShaderEditor::MAX_VERTEX_OUTPUTS_COUNT][50])
{
	blob << "$input ";
	bool first = true;
	for(int i = 0; i < (int)ShaderEditor::VertexInput::COUNT; ++i)
	{
		if(!inputs[i]) continue;

		if(!first) blob << ", ";
		first = false;

		blob << getVertexInputBGFXName((ShaderEditor::VertexInput)i);
	}
	blob << "\n";

	first = true;
	blob << "$output ";
	for(int i = 0; i < ShaderEditor::MAX_VERTEX_OUTPUTS_COUNT; ++i)
	{
		if(!outputs[i][0]) continue;

		if(!first) blob << ", ";
		first = false;

		blob << outputs[i];
	}
	blob << "\n";
}


static void removeConnection(ShaderEditor::Node* node, int pin_index, bool is_input)
{
	if(is_input)
	{
		if(!node->m_inputs[pin_index]) return;

		int index = node->m_inputs[pin_index]->m_outputs.indexOf(node);
		ASSERT(index >= 0);

		node->m_inputs[pin_index]->m_outputs[index] = nullptr;
		node->m_inputs[pin_index] = nullptr;
	}
	else
	{
		if(!node->m_outputs[pin_index]) return;

		int index = node->m_outputs[pin_index]->m_inputs.indexOf(node);
		ASSERT(index >= 0);

		node->m_outputs[pin_index]->m_inputs[index] = nullptr;
		node->m_outputs[pin_index] = nullptr;
	}
}


struct ShaderEditor::ICommand
{
	ICommand(ShaderEditor& editor)
		: m_editor(editor)
	{
	}

	virtual ~ICommand() {}

	virtual void execute() = 0;
	virtual void undo() = 0;
	virtual bool merge(ICommand& command) { return false; }
	virtual uint32_t getType() const = 0;

	ShaderEditor& m_editor;
};


void ShaderEditor::Node::printReference(Lumix::OutputBlob& blob)
{
	blob << "v" << m_id;
}


ShaderEditor::Node::Node(int type, ShaderEditor& editor)
	: m_inputs(editor.m_allocator)
	, m_outputs(editor.m_allocator)
	, m_type(type)
	, m_editor(editor)
{
}


ShaderEditor::ValueType ShaderEditor::Node::getInputType(int index) const
{
	if(!m_inputs[index]) return ShaderEditor::ValueType::NONE;

	int output_idx = m_inputs[index]->m_outputs.indexOf(this);
	return m_inputs[index]->getOutputType(output_idx);
}


void ShaderEditor::Node::onNodeGUI()
{
	ImGui::PushItemWidth(120);
	onGUI();
	ImGui::PopItemWidth();
}


struct MultiplyNode : public ShaderEditor::Node
{
	MultiplyNode(ShaderEditor& editor)
		: Node((int)NodeType::MULTIPLY, editor)
	{
		m_inputs.push(nullptr);
		m_inputs.push(nullptr);
		m_outputs.push(nullptr);
	}


	void save(Lumix::OutputBlob& blob) override {}
	void load(Lumix::InputBlob& blob) override {}


	ShaderEditor::ValueType getOutputType(int) const override
	{
		return m_inputs[1] ? getInputType(1) : ShaderEditor::ValueType::NONE;
	}


	void generate(Lumix::OutputBlob& blob) override
	{
		if(!m_inputs[0]) return;
		if(!m_inputs[1]) return;

		m_inputs[0]->generate(blob);
		m_inputs[1]->generate(blob);

		auto input0_type = getInputType(0);
		bool is_matrix = input0_type == ShaderEditor::ValueType::MATRIX3 ||
						 input0_type == ShaderEditor::ValueType::MATRIX4;
		blob << "\t" << getValueTypeName(getInputType(1)) << " v" << m_id << " = ";

		if(is_matrix) blob << "mul(";

		m_inputs[0]->printReference(blob);
		blob << (is_matrix ? ", " : " * ");
		m_inputs[1]->printReference(blob);
		blob << (is_matrix ? ");\n" : ";\n");
	}

	void onGUI() override
	{
		ImGui::Text("A");
		ImGui::Text("B");
	}
};


struct Vec4MergeNode : public ShaderEditor::Node
{
	Vec4MergeNode(ShaderEditor& editor)
		: Node((int)NodeType::VEC4_MERGE, editor)
	{
		m_inputs.push(nullptr);
		m_inputs.push(nullptr);
		m_inputs.push(nullptr);
		m_inputs.push(nullptr);
		m_inputs.push(nullptr);
		m_outputs.push(nullptr);
	}


	void save(Lumix::OutputBlob& blob) override {}
	void load(Lumix::InputBlob& blob) override {}
	ShaderEditor::ValueType getOutputType(int) const override { return ShaderEditor::ValueType::VEC4; }


	void generate(Lumix::OutputBlob& blob) override 
	{
		blob << "\tvec4 v" << m_id << ";\n";

		if (m_inputs[0]) 
		{
			m_inputs[0]->generate(blob);

			blob << "\tv" << m_id << ".xyz = ";
			m_inputs[0]->printReference(blob);
			blob << ";\n";
		}
		if (m_inputs[1])
		{
			m_inputs[1]->generate(blob);

			blob << "\tv" << m_id << ".x = ";
			m_inputs[1]->printReference(blob);
			blob << ";\n";
		}
		if (m_inputs[2])
		{
			m_inputs[2]->generate(blob);

			blob << "\tv" << m_id << ".y = ";
			m_inputs[2]->printReference(blob);
			blob << ";\n";
		}
		if (m_inputs[3])
		{
			m_inputs[3]->generate(blob);

			blob << "\tv" << m_id << ".z = ";
			m_inputs[3]->printReference(blob);
			blob << ";\n";
		}
		if (m_inputs[4])
		{
			m_inputs[4]->generate(blob);

			blob << "\tv" << m_id << ".w = ";
			m_inputs[4]->printReference(blob);
			blob << ";\n";
		}
	}


	void onGUI() override
	{
		ImGui::Text("xyz");
		ImGui::Text("x");
		ImGui::Text("y");
		ImGui::Text("z");
		ImGui::Text("w");
	}
};


struct FloatConstNode : public ShaderEditor::Node
{
	FloatConstNode(ShaderEditor& editor)
		: Node((int)NodeType::FLOAT_CONST, editor)
	{
		m_value = 0;
		m_outputs.push(nullptr);
	}

	void save(Lumix::OutputBlob& blob) override { blob.write(m_value); }
	void load(Lumix::InputBlob& blob) override { blob.read(m_value); }
	ShaderEditor::ValueType getOutputType(int index) const override { return ShaderEditor::ValueType::FLOAT; }

	void generate(Lumix::OutputBlob& blob) override	{}

	void printReference(Lumix::OutputBlob& blob)
	{
		blob << m_value;
	}

	void onGUI() override { ImGui::DragFloat("value", &m_value, 0.1f); }

	float m_value;
};


struct ColorConstNode : public ShaderEditor::Node
{
	ColorConstNode(ShaderEditor& editor)
		: Node((int)NodeType::COLOR_CONST, editor)
	{
		m_color[0] = m_color[1] = m_color[2] = m_color[3] = 0;
		m_outputs.push(nullptr);
	}

	void save(Lumix::OutputBlob& blob) override { blob.write(m_color); }
	void load(Lumix::InputBlob& blob) override { blob.read(m_color); }
	ShaderEditor::ValueType getOutputType(int index) const override { return ShaderEditor::ValueType::VEC4; }

	void generate(Lumix::OutputBlob& blob) override
	{
		blob << "\tconst vec4 v" << m_id << " = vec4(" << m_color[0] << ", " << m_color[1] << ", "
			 << m_color[2] << ", " << m_color[3] << ");\n";
	}

	void onGUI() override { ImGui::ColorEdit4("value", m_color); }

	float m_color[4];
};


struct SampleNode : public ShaderEditor::Node
{
	SampleNode(ShaderEditor& editor)
		: Node((int)NodeType::SAMPLE, editor)
	{
		m_inputs.push(nullptr);
		m_outputs.push(nullptr);
		m_texture = 0;
	}

	void save(Lumix::OutputBlob& blob) override { blob.write(m_texture); }
	void load(Lumix::InputBlob& blob) override { blob.read(m_texture); }
	ShaderEditor::ValueType getOutputType(int index) const override { return ShaderEditor::ValueType::VEC4; }

	void generate(Lumix::OutputBlob& blob) override
	{
		if (!m_inputs[0])
		{
			blob << "\tvec4 v" << m_id << " = vec4(1, 0, 1, 0);\n";
			return;
		}

		m_inputs[0]->generate(blob);
		blob << "\tvec4 v" << m_id << " = texture2D(" << m_editor.getTextureName(m_texture) << ", ";
		m_inputs[0]->printReference(blob);
		blob << ");\n";
	}

	void onGUI() override
	{
		ImGui::Text("UV");
		auto getter = [](void* data, int idx, const char** out) -> bool
		{
			*out = ((SampleNode*)data)->m_editor.getTextureName(idx);
			return true;
		};
		ImGui::Combo("Texture", &m_texture, getter, this, ShaderEditor::MAX_TEXTURES_COUNT);
	}

	int m_texture;
};


struct VertexInputNode : public ShaderEditor::Node
{
	VertexInputNode(ShaderEditor& editor)
		: Node((int)NodeType::VERTEX_INPUT, editor)
	{
		m_outputs.push(nullptr);
		m_input = ShaderEditor::VertexInput::POSITION;
	}


	void save(Lumix::OutputBlob& blob) override { blob.write((int)m_input); }


	void load(Lumix::InputBlob& blob) override
	{
		int tmp;
		blob.read(tmp);
		m_input = (ShaderEditor::VertexInput)tmp;
	}


	void printReference(Lumix::OutputBlob& blob) override
	{
		for (auto& i : VERTEX_INPUTS)
		{
			if (i.input == m_input)
			{
				blob << i.system_name;
				return;
			}
		}
	}


	ShaderEditor::ValueType getOutputType(int index) const override
	{
		for (auto& input : VERTEX_INPUTS)
		{
			if (input.input == m_input)
			{
				return input.type;
			}
		}

		return ShaderEditor::ValueType::FLOAT;
	}


	void generate(Lumix::OutputBlob& blob) override {}


	void onGUI() override
	{
		auto getter = [](void* data, int idx, const char** out) -> bool
		{
			*out = getVertexInputBGFXName((ShaderEditor::VertexInput)idx);
			return true;
		};
		int input = (int)m_input;
		ImGui::Combo("Input", &input, getter, this, (int)ShaderEditor::VertexInput::COUNT);
		m_input = (ShaderEditor::VertexInput)input;
	}

	ShaderEditor::VertexInput m_input;
};


struct FragmentInputNode : public ShaderEditor::Node
{
	FragmentInputNode(ShaderEditor& editor)
		: Node((int)NodeType::FRAGMENT_INPUT, editor)
	{
		m_outputs.push(nullptr);
		m_attribute = 0;
	}

	void save(Lumix::OutputBlob& blob) override { blob.write(m_attribute); }
	void load(Lumix::InputBlob& blob) override { blob.read(m_attribute); }
	void generate(Lumix::OutputBlob& blob) override {}


	void printReference(Lumix::OutputBlob& blob) override
	{
		blob << m_editor.getVertexOutputName(m_attribute);
	}


	void onGUI() override
	{
		auto getter = [](void* data, int idx, const char** out) -> bool
		{
			*out = ((FragmentInputNode*)data)->m_editor.getVertexOutputName(idx);
			return true;
		};
		ImGui::Combo("Input", &m_attribute, getter, this, ShaderEditor::MAX_VERTEX_OUTPUTS_COUNT);
	}

	int m_attribute;
};



struct VertexOutputNode : public ShaderEditor::Node
{
	VertexOutputNode(ShaderEditor& editor)
		: Node((int)NodeType::VERTEX_OUTPUT, editor)
	{
		m_inputs.push(nullptr);
		m_output_idx = 0;
	}


	void generate(Lumix::OutputBlob& blob) override
	{
		if(!m_inputs[0])
		{
			blob << "\t" << m_editor.getVertexOutputName(m_output_idx) << " = vec4(1.0, 0.0, 1.0, 0.0);";
			return;
		}

		m_inputs[0]->generate(blob);
		blob << "\t" << m_editor.getVertexOutputName(m_output_idx) << " = ";
		m_inputs[0]->printReference(blob);
		blob << ";\n";
	}


	void onGUI() override 
	{
		ImGui::Combo("output",
			&m_output_idx,
			[](void* data, int idx, const char** out_text) -> bool
			{
				auto* node = (VertexOutputNode*)data;
				*out_text = node->m_editor.getVertexOutputName(idx);
				return true;
			},
			this,
			ShaderEditor::MAX_VERTEX_OUTPUTS_COUNT);
	}


	int m_output_idx;
};


struct PositionOutputNode : public ShaderEditor::Node
{
	PositionOutputNode(ShaderEditor& editor)
		: Node((int)NodeType::POSITION_OUTPUT, editor)
	{
		m_inputs.push(nullptr);
	}


	void generate(Lumix::OutputBlob& blob) override
	{
		if(!m_inputs[0])
		{
			blob << "\tgl_Position = vec4(1, 0, 1, 1);\n";
			return;
		}

		m_inputs[0]->generate(blob);
		blob << "\tgl_Position = ";
		m_inputs[0]->printReference(blob);
		blob << ";\n";
	}


	void onGUI() override { ImGui::Text("Output position"); }
};



struct FragmentOutputNode : public ShaderEditor::Node
{
	FragmentOutputNode(ShaderEditor& editor)
		: Node((int)NodeType::FRAGMENT_OUTPUT, editor)
	{
		m_inputs.push(nullptr);
	}


	void generate(Lumix::OutputBlob& blob) override
	{
		if (!m_inputs[0])
		{
			blob << "\tgl_FragColor = vec4(1, 0, 1, 1);\n";
			return;
		}

		m_inputs[0]->generate(blob);
		blob << "\tgl_FragColor = ";
		m_inputs[0]->printReference(blob);
		blob << ";\n";
	}


	void onGUI() override { ImGui::Text("OUTPUT"); }
};


struct MixNode : public ShaderEditor::Node
{
	MixNode(ShaderEditor& editor)
		: Node((int)NodeType::MIX, editor)
	{
		m_inputs.push(nullptr);
		m_inputs.push(nullptr);
		m_inputs.push(nullptr);
		m_outputs.push(nullptr);
	}

	ShaderEditor::ValueType getOutputType(int index) const override 
	{
		return getInputType(1);
	}

	void generate(Lumix::OutputBlob& blob) override
	{
		if (!m_inputs[0] || !m_inputs[1] || !m_inputs[2])
		{
			blob << "\t" << getValueTypeName(getOutputType(0)) << " v" << m_id << ";\n";
			return;
		}

		m_inputs[0]->generate(blob);
		m_inputs[1]->generate(blob);
		m_inputs[2]->generate(blob);

		blob << "\t" << getValueTypeName(getOutputType(0)) << " v" << m_id << " = mix(";
		m_inputs[0]->printReference(blob);
		blob << ", ";
		m_inputs[1]->printReference(blob);
		blob << ", ";
		m_inputs[2]->printReference(blob);
		blob << ");";
	}

	void onGUI() override
	{
		ImGui::Text("Input 1");
		ImGui::Text("Input 2");
		ImGui::Text("Weight");
	}
};


struct BuiltinUniformNode : public ShaderEditor::Node
{
	BuiltinUniformNode(ShaderEditor& editor)
		: Node((int)NodeType::BUILTIN_UNIFORM, editor)
	{
		m_outputs.push(nullptr);
		m_uniform = BuiltinUniform::MODEL_MTX;
	}


	void save(Lumix::OutputBlob& blob) override { blob.write(m_uniform); }
	void load(Lumix::InputBlob& blob) override { blob.read(m_uniform); }


	void printReference(Lumix::OutputBlob& blob) override
	{
		for(auto& u : BUILTIN_UNIFORMS)
		{
			if(u.uniform == m_uniform)
			{
				blob << u.bgfx_name;
				return;
			}
		}
	}


	ShaderEditor::ValueType getOutputType(int) const override
	{
		for(auto& unif : BUILTIN_UNIFORMS)
		{
			if (m_uniform == unif.uniform) return unif.type;
		}

		return ShaderEditor::ValueType::NONE;
	}


	void generateBeforeMain(Lumix::OutputBlob& blob) override {}

	void generate(Lumix::OutputBlob& blob) override {}

	void onGUI() override
	{
		ImGui::Combo("Uniform", (int*)&m_uniform, "Model\0View & Projection\0");
	}

	BuiltinUniform m_uniform;
};



struct UniformNode : public ShaderEditor::Node
{
	UniformNode(ShaderEditor& editor)
		: Node((int)NodeType::UNIFORM, editor)
	{
		m_outputs.push(nullptr);
		m_value_type = ShaderEditor::ValueType::VEC4;
		m_name[0] = 0;
	}

	void save(Lumix::OutputBlob& blob) override { blob.write(m_type); }
	void load(Lumix::InputBlob& blob) override { blob.read(m_type); }
	ShaderEditor::ValueType getOutputType(int) const override { return m_value_type; }


	void printReference(Lumix::OutputBlob& blob) override
	{
		blob << m_name;
	}


	void generateBeforeMain(Lumix::OutputBlob& blob) override
	{
		blob << "uniform " << getValueTypeName(m_value_type) << " " << m_name << ";\n";
	}


	void generate(Lumix::OutputBlob& blob) override {}


	void onGUI() override
	{
		auto getter = [](void*, int idx, const char** out_text) -> bool {
			*out_text = getValueTypeName((ShaderEditor::ValueType)idx);
			return true;
		};
		int tmp = (int)m_value_type;
		ImGui::Combo("Type", &tmp, getter, this, (int)ShaderEditor::ValueType::COUNT);
		m_value_type = (ShaderEditor::ValueType)tmp;
		ImGui::InputText("Name", m_name, sizeof(m_name));
	}

	char m_name[50];
	ShaderEditor::ValueType m_value_type;
};


ShaderEditor::Node::~Node()
{
	for (auto* output : m_outputs)
	{
		if (output) output->m_inputs.eraseItem(this);
	}

	for (auto* input : m_inputs)
	{
		if (input) input->m_outputs.eraseItem(this);
	}
}



struct MoveNodeCommand : public ShaderEditor::ICommand
{
	MoveNodeCommand(int node, ImVec2 new_pos, ShaderEditor& editor)
		: ICommand(editor)
		, m_node(node)
		, m_new_pos(new_pos)
	{
		m_old_pos = m_editor.getNodeByID(m_node)->m_pos;
	}


	virtual uint32_t getType() const override
	{
		static const uint32_t crc = Lumix::crc32("move_node");
		return crc;
	}


	void execute() override
	{
		auto* node = m_editor.getNodeByID(m_node);
		node->m_pos = m_new_pos;
	}


	void undo() override
	{
		auto* node = m_editor.getNodeByID(m_node);
		node->m_pos = m_old_pos;
	}


	bool merge(ICommand& command) override
	{
		auto& cmd = static_cast<MoveNodeCommand&>(command);
		if(cmd.m_node == m_node)
		{

			m_new_pos = cmd.m_new_pos;
			return true;
		}
		return false;
	}


	int m_node;
	ImVec2 m_new_pos;
	ImVec2 m_old_pos;
};


struct CreateConnectionCommand : public ShaderEditor::ICommand
{
	CreateConnectionCommand(int from, int from_pin, int to, int to_pin, ShaderEditor& editor)
		: ICommand(editor)
		, m_from(from)
		, m_to(to)
		, m_from_pin(from_pin)
		, m_to_pin(to_pin)
	{
		auto* from_node = m_editor.getNodeByID(m_from);
		auto* to_node = m_editor.getNodeByID(m_to);

		auto* before_to = from_node->m_outputs[m_from_pin];
		if(before_to)
		{
			m_before_to_pin = before_to->m_inputs.indexOf(from_node);
			m_before_to = before_to->m_id;
		}
		else
		{
			m_before_to = m_before_to_pin = -1;
		}

		auto* before_from = to_node->m_inputs[m_from_pin];
		if(before_from)
		{
			m_before_from_pin = before_from->m_outputs.indexOf(to_node);
			m_before_from = before_from->m_id;
		}
		else
		{
			m_before_from = m_before_from_pin = -1;
		}
	}


	virtual uint32_t getType() const override
	{
		static const uint32_t crc = Lumix::crc32("create_connection");
		return crc;
	}


	virtual void execute() override
	{
		auto* from_node = m_editor.getNodeByID(m_from);
		auto* to_node = m_editor.getNodeByID(m_to);

		removeConnection(from_node, m_from_pin, false);
		removeConnection(to_node, m_to_pin, true);

		from_node->m_outputs[m_from_pin] = to_node;
		to_node->m_inputs[m_to_pin] = from_node;
	}


	virtual void undo() override
	{
		auto* node = m_editor.getNodeByID(m_from);
		removeConnection(node, m_from_pin, false);

		if(m_before_to >= 0)
		{
			auto* before_to = m_editor.getNodeByID(m_before_to);

			node->m_outputs[m_from_pin] = before_to;
			before_to->m_inputs[m_before_to_pin] = node;
		}

		if(m_before_from >= 0)
		{
			auto* before_from = m_editor.getNodeByID(m_before_from);

			auto* to_node = m_editor.getNodeByID(m_to);

			to_node->m_inputs[m_to_pin] = before_from;
			before_from->m_outputs[m_before_from_pin] = to_node;
		}
	}

	int m_before_to;
	int m_before_to_pin;
	int m_before_from;
	int m_before_from_pin;
	int m_from_pin;
	int m_to_pin;
	int m_from;
	int m_to;
};


struct RemoveNodeCommand : public ShaderEditor::ICommand
{
	RemoveNodeCommand(int node_id, ShaderEditor::ShaderType shader_type, ShaderEditor& editor)
		: ICommand(editor)
		, m_node_id(node_id)
		, m_blob(editor.getAllocator())
		, m_shader_type(shader_type)
	{
	}


	virtual uint32_t getType() const override
	{
		static const uint32_t crc = Lumix::crc32("remove_node");
		return crc;
	}


	void execute() override
	{
		auto* node = m_editor.getNodeByID(m_node_id);
		m_editor.saveNode(m_blob, *node);
		m_editor.saveNodeConnections(m_blob, *node);
		m_editor.destroyNode(node);
	}


	void undo() override
	{
		Lumix::InputBlob blob(m_blob);
		auto& node = m_editor.loadNode(blob, m_shader_type);
		m_editor.loadNodeConnections(blob, node);
	}


	ShaderEditor::ShaderType m_shader_type;
	Lumix::OutputBlob m_blob;
	int m_node_id;
};


struct CreateNodeCommand : public ShaderEditor::ICommand
{
	CreateNodeCommand(int id, NodeType type, ShaderEditor::ShaderType shader_type, ImVec2 pos, ShaderEditor& editor)
		: m_type(type)
		, m_pos(pos)
		, m_id(id)
		, m_shader_type(shader_type)
		, ICommand(editor)
	{
	}


	virtual uint32_t getType() const override
	{
		static const uint32_t crc = Lumix::crc32("create_node");
		return crc;
	}


	void execute() override
	{
		m_node = m_editor.createNode((int)m_type);
		m_editor.addNode(m_node, m_pos, m_shader_type);
		if(m_id >= 0) m_node->m_id = m_id;
	}


	void undo() override
	{
		m_id = m_node->m_id;
		m_editor.destroyNode(m_node);
	}


	int m_id;
	ShaderEditor::ShaderType m_shader_type;
	ShaderEditor::Node* m_node;
	NodeType m_type;
	ImVec2 m_pos;
};


ShaderEditor::ShaderEditor(Lumix::IAllocator& allocator)
	: m_fragment_nodes(allocator)
	, m_vertex_nodes(allocator)
	, m_allocator(allocator)
	, m_undo_stack(allocator)
	, m_undo_stack_idx(-1)
	, m_current_node_id(-1)
	, m_is_focused(false)
	, m_current_shader_type(ShaderType::VERTEX)
{
	newGraph();
}


ShaderEditor::~ShaderEditor()
{
	clear();
}


ShaderEditor::Node* ShaderEditor::getNodeByID(int id)
{
	for(auto* node : m_fragment_nodes)
	{
		if(node->m_id == id) return node;
	}

	for(auto* node : m_vertex_nodes)
	{
		if(node->m_id == id) return node;
	}

	return nullptr;
}


void ShaderEditor::generate(const char* path, ShaderType shader_type)
{
	char sc_path[Lumix::MAX_PATH_LENGTH];
	Lumix::PathUtils::FileInfo info(path);
	Lumix::copyString(sc_path, info.m_dir);
	Lumix::catString(sc_path, info.m_basename);
	if(shader_type == ShaderType::FRAGMENT)
	{
		Lumix::catString(sc_path, "_fs.sc");
	}
	else
	{
		Lumix::catString(sc_path, "_vs.sc");
	}

	FILE* fp = fopen(sc_path, "wb");
	if (!fp)
	{
		Lumix::g_log_error.log("Shader editor") << "Could not generate " << sc_path;
		return;
	}

	Lumix::OutputBlob blob(m_allocator);
	blob.reserve(4096);

	if(shader_type == ShaderType::FRAGMENT)
	{
		blob << "$input ";
		bool first = true;
		for(auto* vertex_output : m_vertex_outputs)
		{
			if(!vertex_output[0]) continue;
			if(!first) blob << ", ";
			blob << vertex_output;
			first = false;
		}
		blob << "\n";
	}
	else
	{
		writeVertexShaderHeader(blob, m_vertex_inputs, m_vertex_outputs);
	}

	blob << "#include \"common.sh\"\n";

	for (int i = 0; i < Lumix::lengthOf(m_textures); ++i)
	{
		if (!m_textures[i][0]) continue;

		blob << "SAMPLER2D(" << m_textures[i] << ", " << i << ");\n";
	}

	auto& nodes = shader_type == ShaderType::FRAGMENT ? m_fragment_nodes : m_vertex_nodes;
	for (auto* node : nodes)
	{
		node->generateBeforeMain(blob);
	}

	blob << "void main() {\n";
	for(auto& node : nodes)
	{
		if (node->m_type == (int)NodeType::FRAGMENT_OUTPUT ||
			node->m_type == (int)NodeType::VERTEX_OUTPUT ||
			node->m_type == (int)NodeType::POSITION_OUTPUT)
		{
			node->generate(blob);
		}
	}
	blob << "}\n";

	fwrite(blob.getData(), 1, blob.getSize(), fp);
	fclose(fp);
}


void ShaderEditor::addNode(Node* node, const ImVec2& pos, ShaderType type)
{
	if(type == ShaderType::FRAGMENT)
	{
		m_fragment_nodes.push(node);
	}
	else
	{
		m_vertex_nodes.push(node);
	}

	node->m_pos = pos;
	node->m_id = ++m_last_node_id;
}


void ShaderEditor::nodePinMouseDown(Node* node, int pin_index, bool is_input)
{
	m_new_link_info.from = node;
	m_new_link_info.from_pin_index = pin_index;
	m_new_link_info.is_from_input = is_input;
	m_new_link_info.is_active = true;
}


void ShaderEditor::createConnection(Node* node, int pin_index, bool is_input)
{
	if (!m_new_link_info.is_active) return;
	if (m_new_link_info.is_from_input == is_input) return;

	if (is_input)
	{
		execute(LUMIX_NEW(m_allocator, CreateConnectionCommand)(
			m_new_link_info.from->m_id, m_new_link_info.from_pin_index, node->m_id, pin_index, *this));
	}
	else
	{
		execute(LUMIX_NEW(m_allocator, CreateConnectionCommand)(
			node->m_id, pin_index, m_new_link_info.from->m_id, m_new_link_info.from_pin_index, *this));
	}
}


void ShaderEditor::saveNode(Lumix::OutputBlob& blob, Node& node)
{
	int type = (int)node.m_type;
	blob.write(node.m_id);
	blob.write(type);
	blob.write(node.m_pos);

	node.save(blob);
}


void ShaderEditor::saveNodeConnections(Lumix::OutputBlob& blob, Node& node)
{
	int inputs_count = node.m_inputs.size();
	blob.write(inputs_count);
	for(int i = 0; i < inputs_count; ++i)
	{
		int tmp = node.m_inputs[i] ? node.m_inputs[i]->m_id : -1;
		blob.write(tmp);
		tmp = node.m_inputs[i] ? node.m_inputs[i]->m_outputs.indexOf(&node) : -1;
		blob.write(tmp);
	}

	int outputs_count = node.m_outputs.size();
	blob.write(outputs_count);
	for(int i = 0; i < outputs_count; ++i)
	{
		int tmp = node.m_outputs[i] ? node.m_outputs[i]->m_id : -1;
		blob.write(tmp);
		tmp = node.m_outputs[i] ? node.m_outputs[i]->m_inputs.indexOf(&node) : -1;
		blob.write(tmp);
	}
}


void ShaderEditor::save(const char* path)
{
	FILE* fp = fopen(path, "wb");
	if (!fp)
	{
		Lumix::g_log_error.log("Shader editor") << "Could not save shader " << path;
		return;
	}

	Lumix::OutputBlob blob(m_allocator);
	blob.reserve(4096);
	for (int i = 0; i < Lumix::lengthOf(m_textures); ++i)
	{
		blob.writeString(m_textures[i]);
	}

	for (int i = 0; i < Lumix::lengthOf(m_vertex_outputs); ++i)
	{
		blob.writeString(m_vertex_outputs[i]);
	}

	blob.write(m_vertex_inputs, sizeof(m_vertex_inputs));

	int nodes_count = m_vertex_nodes.size();
	blob.write(nodes_count);
	for(auto* node : m_vertex_nodes)
	{
		saveNode(blob, *node);
	}

	for(auto* node : m_vertex_nodes)
	{
		saveNodeConnections(blob, *node);
	}

	nodes_count = m_fragment_nodes.size();
	blob.write(nodes_count);
	for (auto* node : m_fragment_nodes)
	{
		saveNode(blob, *node);
	}

	for (auto* node : m_fragment_nodes)
	{
		saveNodeConnections(blob, *node);
	}

	fwrite(blob.getData(), blob.getSize(), 1, fp);
	fclose(fp);
}


void ShaderEditor::clear()
{
	for (auto* node : m_fragment_nodes)
	{
		LUMIX_DELETE(m_allocator, node);
	}
	m_fragment_nodes.clear();

	for(auto* node : m_vertex_nodes)
	{
		LUMIX_DELETE(m_allocator, node);
	}
	m_vertex_nodes.clear();

	for(auto* command : m_undo_stack)
	{
		LUMIX_DELETE(m_allocator, command);
	}
	m_undo_stack.clear();
	m_undo_stack_idx = -1;

	m_last_node_id = 0;
}


ShaderEditor::Node* ShaderEditor::createNode(int type)
{
	switch ((NodeType)type)
	{
		case NodeType::FRAGMENT_OUTPUT:				return LUMIX_NEW(m_allocator, FragmentOutputNode)(*this);
		case NodeType::VERTEX_OUTPUT:					return LUMIX_NEW(m_allocator, VertexOutputNode)(*this);
		case NodeType::FRAGMENT_INPUT:				return LUMIX_NEW(m_allocator, FragmentInputNode)(*this);
		case NodeType::POSITION_OUTPUT:				return LUMIX_NEW(m_allocator, PositionOutputNode)(*this);
		case NodeType::VERTEX_INPUT:					return LUMIX_NEW(m_allocator, VertexInputNode)(*this);
		case NodeType::COLOR_CONST:						return LUMIX_NEW(m_allocator, ColorConstNode)(*this);
		case NodeType::FLOAT_CONST:						return LUMIX_NEW(m_allocator, FloatConstNode)(*this);
		case NodeType::MIX:										return LUMIX_NEW(m_allocator, MixNode)(*this);
		case NodeType::SAMPLE:								return LUMIX_NEW(m_allocator, SampleNode)(*this);
		case NodeType::UNIFORM:								return LUMIX_NEW(m_allocator, UniformNode)(*this);
		case NodeType::VEC4_MERGE:						return LUMIX_NEW(m_allocator, Vec4MergeNode)(*this);
		case NodeType::MULTIPLY:							return LUMIX_NEW(m_allocator, MultiplyNode)(*this);
		case NodeType::BUILTIN_UNIFORM:				return LUMIX_NEW(m_allocator, BuiltinUniformNode)(*this);
	}

	ASSERT(false);
	return nullptr;
}


ShaderEditor::Node& ShaderEditor::loadNode(Lumix::InputBlob& blob, ShaderType shader_type)
{
	int type;
	int id;
	blob.read(id);
	blob.read(type);
	Node* node = createNode(type);
	node->m_id = id;
	if(shader_type == ShaderType::FRAGMENT)
	{
		m_fragment_nodes.push(node);
	}
	else
	{
		m_vertex_nodes.push(node);
	}
	blob.read(node->m_pos);

	node->load(blob);
	return *node;
}


void ShaderEditor::loadNodeConnections(Lumix::InputBlob& blob, Node& node)
{
	int size;
	blob.read(size);
	for(int i = 0; i < size; ++i)
	{
		int tmp;
		blob.read(tmp);
		node.m_inputs[i] = tmp < 0 ? nullptr : getNodeByID(tmp);
		blob.read(tmp);
		if(node.m_inputs[i]) node.m_inputs[i]->m_outputs[tmp] = &node;
	}

	blob.read(size);
	for(int i = 0; i < size; ++i)
	{
		int tmp;
		blob.read(tmp);
		node.m_outputs[i] = tmp < 0 ? nullptr : getNodeByID(tmp);
		blob.read(tmp);
		if(node.m_outputs[i]) node.m_outputs[i]->m_inputs[tmp] = &node;
	}
}


void ShaderEditor::load()
{
	char path[Lumix::MAX_PATH_LENGTH];
	if (!Lumix::getOpenFilename(path, Lumix::lengthOf(path), "Shader edit data\0*.sed\0"))
	{
		return;
	}
	m_path = path;

	clear();

	FILE* fp = fopen(path, "rb");
	if (!fp)
	{
		Lumix::g_log_error.log("Shader editor") << "Failed to load shader " << path;
		return;
	}

	fseek(fp, 0, SEEK_END);
	int data_size = (int)ftell(fp);
	Lumix::Array<uint8_t> data(m_allocator);
	data.resize(data_size);
	fseek(fp, 0, SEEK_SET);
	fread(&data[0], 1, data_size, fp);

	Lumix::InputBlob blob(&data[0], data_size);
	for (int i = 0; i < Lumix::lengthOf(m_textures); ++i)
	{
		blob.readString(m_textures[i], Lumix::lengthOf(m_textures[i]));
	}

	for (int i = 0; i < Lumix::lengthOf(m_vertex_outputs); ++i)
	{
		blob.readString(m_vertex_outputs[i], Lumix::lengthOf(m_vertex_outputs[i]));
	}

	blob.read(m_vertex_inputs, sizeof(m_vertex_inputs));

	int size;
	blob.read(size);
	for(int i = 0; i < size; ++i)
	{
		loadNode(blob, ShaderType::VERTEX);
	}

	for(auto* node : m_vertex_nodes)
	{
		loadNodeConnections(blob, *node);
		m_last_node_id = Lumix::Math::maxValue(int(node->m_id + 1), int(m_last_node_id));
	}

	blob.read(size);
	for (int i = 0; i < size; ++i)
	{
		loadNode(blob, ShaderType::FRAGMENT);
	}

	for (auto* node : m_fragment_nodes)
	{
		loadNodeConnections(blob, *node);
		m_last_node_id = Lumix::Math::maxValue(int(node->m_id + 1), int(m_last_node_id));
	}

	fclose(fp);
}


void ShaderEditor::getSavePath()
{
	char path[Lumix::MAX_PATH_LENGTH];
	Lumix::getSaveFilename(path, Lumix::lengthOf(path), "Shader edit data\0*.sed\0", "sed");
	m_path = path;
}


static ImVec2 operator+(const ImVec2& a, const ImVec2& b)
{
	return ImVec2(a.x + b.x, a.y + b.y);
}


static ImVec2 operator-(const ImVec2& a, const ImVec2& b)
{
	return ImVec2(a.x - b.x, a.y - b.y);
}


void ShaderEditor::onGUIRightColumn()
{
	ImGui::BeginChild("right_col");

	if(ImGui::IsWindowHovered() && !ImGui::IsAnyItemActive() && ImGui::IsMouseDragging(2, 0.0f))
	{
		m_canvas_pos = m_canvas_pos + ImGui::GetIO().MouseDelta;
	}

	int current_shader = (int)m_current_shader_type;
	if(ImGui::Combo("Shader", &current_shader, "Vertex\0Fragment\0"))
	{
		m_current_shader_type = (ShaderType)current_shader;
	}

	auto cursor_screen_pos = ImGui::GetCursorScreenPos();

	auto& nodes = m_current_shader_type == ShaderType::FRAGMENT ? m_fragment_nodes : m_vertex_nodes;
	for(auto* node : nodes)
	{
		auto node_screen_pos = cursor_screen_pos + node->m_pos + m_canvas_pos;

		ImGui::BeginNode(node->m_id, node_screen_pos);
		node->onNodeGUI();
		ImGui::EndNode(node_screen_pos);
		if(ImGui::IsItemHovered() && ImGui::IsMouseDown(1))
		{
			m_current_node_id = node->m_id;
		}

		for(int i = 0; i < node->m_outputs.size(); ++i)
		{
			Node* output = node->m_outputs[i];
			if(!output) continue;

			auto output_screen_pos = cursor_screen_pos + output->m_pos + m_canvas_pos;

			auto output_pos = ImGui::GetNodeOutputPos(node->m_id, i);
			auto input_pos = ImGui::GetNodeInputPos(output->m_id, output->m_inputs.indexOf(node));
			ImGui::NodeLink(output_pos, input_pos);
		}

		for(int i = 0; i < node->m_outputs.size(); ++i)
		{
			auto pin_pos = ImGui::GetNodeOutputPos(node->m_id, i);
			if(ImGui::NodePin(i, pin_pos))
			{
				if(ImGui::IsMouseReleased(0) && m_new_link_info.is_active)
				{
					createConnection(node, i, false);
				}
				if(ImGui::IsMouseClicked(0)) nodePinMouseDown(node, i, false);
			}
		}

		for(int i = 0; i < node->m_inputs.size(); ++i)
		{
			auto pin_pos = ImGui::GetNodeInputPos(node->m_id, i);
			if(ImGui::NodePin(i + node->m_outputs.size(), pin_pos))
			{
				if(ImGui::IsMouseReleased(0) && m_new_link_info.is_active)
				{
					createConnection(node, i, true);
				}
				if(ImGui::IsMouseClicked(0)) nodePinMouseDown(node, i, true);
			}
		}

		ImVec2 new_pos = node_screen_pos - cursor_screen_pos - m_canvas_pos;
		if(new_pos.x != node->m_pos.x || new_pos.y != node->m_pos.y)
		{
			execute(LUMIX_NEW(m_allocator, MoveNodeCommand)(node->m_id, new_pos, *this));
		}
	}

	if(m_new_link_info.is_active && ImGui::IsMouseDown(0))
	{
		if(m_new_link_info.is_from_input)
		{
			auto pos = ImGui::GetNodeInputPos(
				m_new_link_info.from->m_id, m_new_link_info.from_pin_index);
			ImGui::NodeLink(ImGui::GetMousePos(), pos);
		}
		else
		{
			auto pos = ImGui::GetNodeOutputPos(
				m_new_link_info.from->m_id, m_new_link_info.from_pin_index);
			ImGui::NodeLink(pos, ImGui::GetMousePos());
		}
	}
	else
	{
		m_new_link_info.is_active = false;
	}

	if(ImGui::IsMouseClicked(1))
	{
		ImGui::OpenPopup("context_menu");
	}

	if(ImGui::BeginPopup("context_menu"))
	{
		ImVec2 add_pos(ImGui::GetMousePos().x - cursor_screen_pos.x,
			ImGui::GetMousePos().y - cursor_screen_pos.y);
		if(m_current_node_id >= 0)
		{
			if(ImGui::MenuItem("Remove"))
			{
				execute(LUMIX_NEW(m_allocator, RemoveNodeCommand)(m_current_node_id, m_current_shader_type, *this));
			}
		}

		if (ImGui::BeginMenu("Add"))
		{
			for (auto node_type : NODE_TYPES)
			{
				if (!node_type.is_frag && m_current_shader_type == ShaderType::FRAGMENT) continue;
				if (!node_type.is_vert && m_current_shader_type == ShaderType::VERTEX) continue;

				if (ImGui::MenuItem(node_type.name))
				{
					execute(LUMIX_NEW(m_allocator, CreateNodeCommand)(
						-1, node_type.type, m_current_shader_type, add_pos, *this));
				}
			}
			ImGui::EndMenu();
		}

		ImGui::EndPopup();
	}
	ImGui::EndChild();
}


void ShaderEditor::onGUILeftColumn()
{
	ImGui::BeginChild("left_col", ImVec2(120, 0));
	ImGui::PushItemWidth(120);

	ImGui::Text("Vertex inputs");
	for(int i = 0; i < (int)VertexInput::COUNT; ++i)
	{
		ImGui::Checkbox(getVertexInputName((VertexInput)i), &m_vertex_inputs[i]);
	}

	ImGui::Separator();

	ImGui::Text("Vertex outputs");
	for(int i = 0; i < Lumix::lengthOf(m_vertex_outputs); ++i)
	{
		ImGui::InputText(
			StringBuilder<10>("###vout", i), m_vertex_outputs[i], sizeof(m_vertex_outputs[i]));
	}

	ImGui::Separator();
	ImGui::Text("Textures");
	ImGui::Separator();
	for (int i = 0; i < Lumix::lengthOf(m_textures); ++i)
	{
		ImGui::InputText(StringBuilder<10>("###tex", i), m_textures[i], sizeof(m_textures[i]));
	}

	ImGui::PopItemWidth();
	ImGui::EndChild();
}


void ShaderEditor::execute(ICommand* command)
{
	for (int i = m_undo_stack.size() - 1; i > m_undo_stack_idx; --i)
	{
		LUMIX_DELETE(m_allocator, m_undo_stack[i]);
		m_undo_stack.pop();
	}

	if(m_undo_stack_idx >= 0)
	{
		if(m_undo_stack[m_undo_stack_idx]->merge(*command))
		{
			m_undo_stack[m_undo_stack_idx]->execute();
			LUMIX_DELETE(m_allocator, command);
			return;
		}
	}

	m_undo_stack_idx = m_undo_stack.size();
	m_undo_stack.push(command);

	command->execute();
}


bool ShaderEditor::canUndo() const
{
	return m_undo_stack_idx >= 0;
}


bool ShaderEditor::canRedo() const
{
	return m_undo_stack_idx < m_undo_stack.size() - 1;
}


void ShaderEditor::undo()
{
	if (m_undo_stack_idx < 0) return;

	m_undo_stack[m_undo_stack_idx]->undo();
	--m_undo_stack_idx;
}


void ShaderEditor::redo()
{
	if (m_undo_stack_idx + 1 >= m_undo_stack.size()) return;

	m_undo_stack[m_undo_stack_idx + 1]->execute();
	++m_undo_stack_idx;
}


void ShaderEditor::destroyNode(Node* node)
{
	for(auto* input : node->m_inputs)
	{
		if(!input) continue;
		input->m_outputs[input->m_outputs.indexOf(node)] = nullptr;
	}

	for(auto* output : node->m_outputs)
	{
		if(!output) continue;
		output->m_inputs[output->m_inputs.indexOf(node)] = nullptr;
	}

	LUMIX_DELETE(m_allocator, node);
	m_fragment_nodes.eraseItem(node);
	m_vertex_nodes.eraseItem(node);
}


void ShaderEditor::newGraph()
{
	clear();

	for (int i = 0; i < Lumix::lengthOf(m_textures); ++i)
	{
		m_textures[i][0] = 0;
	}
	for (auto& input : m_vertex_inputs)
	{
		input = false;
	}
	for (int i = 0; i < Lumix::lengthOf(m_vertex_outputs); ++i)
	{
		m_vertex_outputs[i][0] = 0;
	}
	m_last_node_id = 0;
	m_new_link_info.is_active = false;

	m_fragment_nodes.push(LUMIX_NEW(m_allocator, FragmentOutputNode)(*this));
	m_fragment_nodes.back()->m_pos.x = 50;
	m_fragment_nodes.back()->m_pos.y = 50;
	m_fragment_nodes.back()->m_id = ++m_last_node_id;

	m_vertex_nodes.push(LUMIX_NEW(m_allocator, PositionOutputNode)(*this));
	m_vertex_nodes.back()->m_pos.x = 50;
	m_vertex_nodes.back()->m_pos.y = 50;
	m_vertex_nodes.back()->m_id = ++m_last_node_id;
}


void ShaderEditor::generateMain(const char* path)
{
	char shd_path[Lumix::MAX_PATH_LENGTH];
	Lumix::PathUtils::FileInfo info(path);
	Lumix::copyString(shd_path, info.m_dir);
	Lumix::catString(shd_path, info.m_basename);
	Lumix::catString(shd_path, ".shd");

	FILE* blob = fopen(shd_path, "wb");
	if(!blob) 
	{
		Lumix::g_log_error.log("Shader editor") << "Could not generate " << shd_path;
		return;
	}

	fputs("passes = {\"MAIN\"}\n"
		  "vs_combinations = {\"\"}\n"
		  "fs_combinations = {\"\"}\n"
		  "texture_slots = {\n",
		blob);

	bool first = true;
	for(const auto& texture : m_textures)
	{
		if(!texture[0]) continue;

		if(!first) fputs(", ", blob);
		first = false;
		fprintf(blob, "{ name = \"%s\", uniform = \"%s\" }", texture, texture);
	}

	fputs("}\n", blob);

	fclose(blob);
}


void ShaderEditor::onGUIMenu()
{
	if(ImGui::BeginMenuBar())
	{
		if(ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("New")) newGraph();
			if (ImGui::MenuItem("Open")) load();
			if (ImGui::MenuItem("Save", nullptr, false, m_path.isValid())) save(m_path.c_str());
			if (ImGui::MenuItem("Save as"))
			{
				getSavePath();
				if (m_path.isValid()) save(m_path.c_str());
			}
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Edit"))
		{
			if (ImGui::MenuItem("Undo", nullptr, false, canUndo())) undo();
			if (ImGui::MenuItem("Redo", nullptr, false, canRedo())) redo();
			ImGui::EndMenu();
		}
		if (ImGui::MenuItem("Generate", nullptr, false, m_path.isValid()))
		{
			generate(m_path.c_str(), ShaderType::VERTEX);
			generate(m_path.c_str(), ShaderType::FRAGMENT);
			generateMain(m_path.c_str());
		}

		ImGui::EndMenuBar();
	}
}


void ShaderEditor::onGUI()
{
	if (ImGui::Begin("Shader editor", nullptr, ImGuiWindowFlags_MenuBar))
	{
		m_is_focused = ImGui::IsRootWindowOrAnyChildFocused();

		onGUIMenu();
		onGUILeftColumn();
		ImGui::SameLine();
		onGUIRightColumn();
	}
	ImGui::End();
}