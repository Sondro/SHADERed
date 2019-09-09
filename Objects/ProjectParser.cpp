#include "ProjectParser.h"
#include "RenderEngine.h"
#include "ObjectManager.h"
#include "PipelineManager.h"
#include "SystemVariableManager.h"
#include "FunctionVariableManager.h"
#include "HLSL2GLSL.h"
#include "Names.h"
#include "Logger.h"

#include "../UI/PinnedUI.h"
#include "../UI/PropertyUI.h"
#include "../UI/PipelineUI.h"
#include "../UI/CodeEditorUI.h"
#include "../Engine/GeometryFactory.h"

#include <fstream>
#include <ghc/filesystem.hpp>

#define HARRAYSIZE(a) (sizeof(a)/sizeof(*a))

namespace ed
{
	std::string toGenericPath(const std::string& p)
	{
		std::string ret = p;
		std::replace(ret.begin(), ret.end(), '\\', '/');
		return ret;
	}

	ProjectParser::ProjectParser(PipelineManager* pipeline, ObjectManager* objects, RenderEngine* rend, MessageStack* msgs, GUIManager* gui) :
		m_pipe(pipeline), m_file(""), m_renderer(rend), m_objects(objects), m_msgs(msgs)
	{

		ResetProjectDirectory();
		m_ui = gui;
	}
	ProjectParser::~ProjectParser()
	{}
	void ProjectParser::Open(const std::string & file)
	{
		Logger::Get().Log("Openning a project file " + file);

		m_file = file;
		SetProjectDirectory(file.substr(0, file.find_last_of("/\\")));

		m_msgs->Clear();

		for (auto& mdl : m_models) {
			if (mdl.second) {
				delete mdl.second;
				mdl.second = nullptr;
			}
		}
		m_models.clear();

		pugi::xml_document doc;
		pugi::xml_parse_result result = doc.load_file(file.c_str());
		if (!result) {
			Logger::Get().Log("Failed to parse a project file", true);
			return;
		}

		m_pipe->Clear();
		m_objects->Clear();

		Settings::Instance().Project.FPCamera = false;
		Settings::Instance().Project.ClearColor = glm::vec4(0, 0, 0, 0);

		pugi::xml_node projectNode = doc.child("project");
		int projectVersion = 1; // if no project version is specified == using first project file
		if (!projectNode.attribute("version").empty())
			projectVersion = projectNode.attribute("version").as_int();

		switch (projectVersion) {
			case 1: m_parseV1(projectNode); break;
			case 2: m_parseV2(projectNode); break;
			default: 
				Logger::Get().Log("Tried to open a project that is newer version", true);
			break;
		}

		// reset time, frame index, etc...
		SystemVariableManager::Instance().Reset();
	
		Logger::Get().Log("Finished with parsing a project file");
	}
	void ProjectParser::OpenTemplate()
	{
		Open(ghc::filesystem::current_path().native() + "/templates/" + m_template + "/template.sprj");
		m_file = ""; // disallow overwriting template.sprj project file
	}
	void ProjectParser::Save()
	{
		SaveAs(m_file);
	}
	void ProjectParser::SaveAs(const std::string & file, bool copyFiles)
	{
		Logger::Get().Log("Saving project file...");

		m_file = file;
		std::string oldProjectPath = m_projectPath;
		SetProjectDirectory(file.substr(0, file.find_last_of("/\\")));

		std::vector<PipelineItem*> passItems = m_pipe->GetList();
		std::vector<pipe::ShaderPass*> collapsedSP = ((PipelineUI*)m_ui->Get(ViewID::Pipeline))->GetCollapsedItems();

		// copy shader files to a directory
		std::string shadersDir = m_projectPath + "/shaders";
		if (copyFiles) {
			Logger::Get().Log("Copying shader files...");

			ghc::filesystem::create_directories(shadersDir);
			std::error_code errc;

			std::string proj = oldProjectPath + ((oldProjectPath[oldProjectPath.size() - 1] == '/') ? "" : "/");
			
			for (PipelineItem* passItem : passItems) {
				pipe::ShaderPass* passData = (pipe::ShaderPass*)passItem->Data;

				std::string vs = proj + std::string(passData->VSPath);
				std::string ps = proj + std::string(passData->PSPath);

				std::string vsExt = ed::HLSL2GLSL::IsHLSL(vs) ? Settings::Instance().General.HLSLExtensions[0] : "glsl";
				std::string psExt = ed::HLSL2GLSL::IsHLSL(ps) ? Settings::Instance().General.HLSLExtensions[0] : "glsl";

				ghc::filesystem::copy_file(vs, shadersDir + "/" + passItem->Name + "VS." + vsExt, ghc::filesystem::copy_options::overwrite_existing, errc);
				ghc::filesystem::copy_file(ps, shadersDir + "/" + passItem->Name + "PS." + psExt, ghc::filesystem::copy_options::overwrite_existing, errc);

				if (passData->GSUsed) {
					std::string gs = proj + std::string(passData->GSPath);
					std::string gsExt = ed::HLSL2GLSL::IsHLSL(gs) ? Settings::Instance().General.HLSLExtensions[0] : "glsl";

					ghc::filesystem::copy_file(gs, shadersDir + "/" + passItem->Name + "GS." + gsExt, ghc::filesystem::copy_options::overwrite_existing, errc);
				}

				if (errc)
					ed::Logger::Get().Log("Failed to copy a file (source == destination)", true);
			}
		}

		pugi::xml_document doc;
		pugi::xml_node projectNode = doc.append_child("project");
		projectNode.append_attribute("version").set_value(2);
		pugi::xml_node pipelineNode = projectNode.append_child("pipeline");
		pugi::xml_node objectsNode = projectNode.append_child("objects");
		pugi::xml_node settingsNode = projectNode.append_child("settings");

		// shader passes
		for (PipelineItem* passItem : passItems) {
			pipe::ShaderPass* passData = (pipe::ShaderPass*)passItem->Data;

			pugi::xml_node passNode = pipelineNode.append_child("pass");
			passNode.append_attribute("name").set_value(passItem->Name);

			/* collapsed="true" attribute */
			for (int i = 0; i < collapsedSP.size(); i++)
				if (collapsedSP[i] == passData) {
					passNode.append_attribute("collapsed").set_value(true);
					break;
				}

			// vertex shader
			pugi::xml_node vsNode = passNode.append_child("shader");
			std::string relativePath = GetRelativePath(oldProjectPath + ((oldProjectPath[oldProjectPath.size() - 1] == '/') ? "" : "/") + std::string(passData->VSPath));
			if (copyFiles) {
				std::string vsExt = ed::HLSL2GLSL::IsHLSL(passData->VSPath) ? Settings::Instance().General.HLSLExtensions[0] : "glsl";
				relativePath = "shaders/" + std::string(passItem->Name) + "VS." + vsExt;
			}

			vsNode.append_attribute("type").set_value("vs");
			vsNode.append_attribute("path").set_value(relativePath.c_str());
			vsNode.append_attribute("entry").set_value(passData->VSEntry);

			// pixel shader
			pugi::xml_node psNode = passNode.append_child("shader");
			relativePath = GetRelativePath(oldProjectPath + ((oldProjectPath[oldProjectPath.size() - 1] == '/') ? "" : "/") + std::string(passData->PSPath));
			if (copyFiles) {
				std::string psExt = ed::HLSL2GLSL::IsHLSL(passData->PSPath) ? Settings::Instance().General.HLSLExtensions[0] : "glsl";
				relativePath = "shaders/" + std::string(passItem->Name) + "PS." + psExt;
			}

			psNode.append_attribute("type").set_value("ps");
			psNode.append_attribute("path").set_value(relativePath.c_str());
			psNode.append_attribute("entry").set_value(passData->PSEntry);

			// geometry shader
			if (strlen(passData->GSEntry) > 0 && strlen(passData->GSPath) > 0) {
				pugi::xml_node gsNode = passNode.append_child("shader");
				relativePath = GetRelativePath(oldProjectPath + ((oldProjectPath[oldProjectPath.size() - 1] == '/') ? "" : "/") + std::string(passData->GSPath));
				if (copyFiles) {
					std::string vsExt = ed::HLSL2GLSL::IsHLSL(passData->GSPath) ? Settings::Instance().General.HLSLExtensions[0] : "glsl";
					relativePath = "shaders/" + std::string(passItem->Name) + "GS." + vsExt;
				}

				gsNode.append_attribute("used").set_value(passData->GSUsed);

				gsNode.append_attribute("type").set_value("gs");
				gsNode.append_attribute("path").set_value(relativePath.c_str());
				gsNode.append_attribute("entry").set_value(passData->GSEntry);
			}

			/* render textures */
			for (int i = 0; i < MAX_RENDER_TEXTURES; i++) {
				if (passData->RenderTextures[i] == 0)
					break;
				GLuint rtID = passData->RenderTextures[i];
				if (rtID == m_renderer->GetTexture())
					passNode.append_child("rendertexture");
				else
					passNode.append_child("rendertexture").append_attribute("name").set_value(m_objects->GetRenderTexture(rtID)->Name.c_str());
			}

			// variables -> now global in pass element [V2]
			m_exportShaderVariables(passNode, passData->Variables.GetVariables());

			// macros
			pugi::xml_node macrosNode = passNode.append_child("macros");		
			for (auto& macro : passData->Macros) {
				pugi::xml_node macroNode = macrosNode.append_child("define");
				macroNode.append_attribute("name").set_value(macro.Name);
				macroNode.append_attribute("active").set_value(macro.Active);
				macroNode.text().set(macro.Value);
			}

			// pass items
			pugi::xml_node itemsNode = passNode.append_child("items");
			for (PipelineItem* item : passData->Items) {
				pugi::xml_node itemNode = itemsNode.append_child("item");
				itemNode.append_attribute("name").set_value(item->Name);

				if (item->Type == PipelineItem::ItemType::Geometry) {
					itemNode.append_attribute("type").set_value("geometry");

					ed::pipe::GeometryItem* tData = reinterpret_cast<ed::pipe::GeometryItem*>(item->Data);

					itemNode.append_child("type").text().set(GEOMETRY_NAMES[tData->Type]);
					itemNode.append_child("width").text().set(tData->Size.x);
					itemNode.append_child("height").text().set(tData->Size.y);
					itemNode.append_child("depth").text().set(tData->Size.z);
					if (tData->Scale.x != 1.0f) itemNode.append_child("scaleX").text().set(tData->Scale.x);
					if (tData->Scale.y != 1.0f) itemNode.append_child("scaleY").text().set(tData->Scale.y);
					if (tData->Scale.z != 1.0f) itemNode.append_child("scaleZ").text().set(tData->Scale.z);
					if (tData->Rotation.z != 0.0f) itemNode.append_child("roll").text().set(tData->Rotation.z);
					if (tData->Rotation.x != 0.0f) itemNode.append_child("pitch").text().set(tData->Rotation.x);
					if (tData->Rotation.y != 0.0f) itemNode.append_child("yaw").text().set(tData->Rotation.y);
					if (tData->Position.x != 0.0f) itemNode.append_child("x").text().set(tData->Position.x);
					if (tData->Position.y != 0.0f) itemNode.append_child("y").text().set(tData->Position.y);
					if (tData->Position.z != 0.0f) itemNode.append_child("z").text().set(tData->Position.z);
					for (int tind = 0; tind < HARRAYSIZE(TOPOLOGY_ITEM_VALUES); tind++) {
						if (TOPOLOGY_ITEM_VALUES[tind] == tData->Topology) {
							itemNode.append_child("topology").text().set(TOPOLOGY_ITEM_NAMES[tind]);
							break;
						}
					}
				}
				else if (item->Type == PipelineItem::ItemType::RenderState) {
					itemNode.append_attribute("type").set_value("renderstate");

					ed::pipe::RenderState* s = reinterpret_cast<ed::pipe::RenderState*>(item->Data);

					if (s->PolygonMode != GL_FILL) itemNode.append_child("wireframe").text().set(s->PolygonMode == GL_LINE);
					if (!s->CullFace) itemNode.append_child("cull").text().set(s->CullFace);
					if (s->CullFaceType != GL_BACK) itemNode.append_child("cullfront").text().set(true);
					if (s->FrontFace != GL_CCW) itemNode.append_child("ccw").text().set(false);

					if (s->Blend) {
						itemNode.append_child("blend").text().set(true);

						if (s->AlphaToCoverage) itemNode.append_child("alpha2coverage").text().set(true);

						itemNode.append_child("colorsrcfactor").text().set(gl::String::BlendFactor(s->BlendSourceFactorRGB));
						itemNode.append_child("colordstfactor").text().set(gl::String::BlendFactor(s->BlendDestinationFactorRGB));
						itemNode.append_child("colorfunc").text().set(gl::String::BlendFunction(s->BlendFunctionColor));
						itemNode.append_child("alphasrcfactor").text().set(gl::String::BlendFactor(s->BlendSourceFactorAlpha));
						itemNode.append_child("alphadstfactor").text().set(gl::String::BlendFactor(s->BlendDestinationFactorAlpha));
						itemNode.append_child("alphafunc").text().set(gl::String::BlendFunction(s->BlendFunctionAlpha));
						itemNode.append_child("blendfactor_r").text().set(s->BlendFactor.r);
						itemNode.append_child("blendfactor_g").text().set(s->BlendFactor.g);
						itemNode.append_child("blendfactor_b").text().set(s->BlendFactor.b);
						itemNode.append_child("blendfactor_a").text().set(s->BlendFactor.a);
					}

					if (s->DepthTest) {
						itemNode.append_child("depthtest").text().set(true);
						itemNode.append_child("depthclamp").text().set(s->DepthClamp);
						itemNode.append_child("depthmask").text().set(s->DepthMask);
						itemNode.append_child("depthfunc").text().set(gl::String::ComparisonFunction(s->DepthFunction));
						itemNode.append_child("depthbias").text().set(s->DepthBias);
					}

					if (s->StencilTest) {
						itemNode.append_child("stenciltest").text().set(true);
						itemNode.append_child("stencilmask").text().set(s->StencilMask);
						itemNode.append_child("stencilref").text().set(s->StencilReference);
						itemNode.append_child("stencilfrontfunc").text().set(gl::String::ComparisonFunction(s->StencilFrontFaceFunction));
						itemNode.append_child("stencilbackfunc").text().set(gl::String::ComparisonFunction(s->StencilBackFaceFunction));
						itemNode.append_child("stencilfrontpass").text().set(gl::String::StencilOperation(s->StencilFrontFaceOpPass));
						itemNode.append_child("stencilbackpass").text().set(gl::String::StencilOperation(s->StencilBackFaceOpPass));
						itemNode.append_child("stencilfrontfail").text().set(gl::String::StencilOperation(s->StencilFrontFaceOpStencilFail));
						itemNode.append_child("stencilbackfail").text().set(gl::String::StencilOperation(s->StencilBackFaceOpStencilFail));
						itemNode.append_child("depthfrontfail").text().set(gl::String::StencilOperation(s->StencilFrontFaceOpDepthFail));
						itemNode.append_child("depthbackfail").text().set(gl::String::StencilOperation(s->StencilBackFaceOpDepthFail));
					}
				}
				else if (item->Type == PipelineItem::ItemType::Model) {
					itemNode.append_attribute("type").set_value("model");

					ed::pipe::Model* data = reinterpret_cast<ed::pipe::Model*>(item->Data);

					std::string opath = GetRelativePath(oldProjectPath + ((oldProjectPath[oldProjectPath.size() - 1] == '/') ? "" : "/") + std::string(data->Filename));;

					itemNode.append_child("filepath").text().set(opath.c_str());
					itemNode.append_child("grouponly").text().set(data->OnlyGroup);
					if (data->OnlyGroup) itemNode.append_child("group").text().set(data->GroupName);
					if (data->Scale.x != 1.0f) itemNode.append_child("scaleX").text().set(data->Scale.x);
					if (data->Scale.y != 1.0f) itemNode.append_child("scaleY").text().set(data->Scale.y);
					if (data->Scale.z != 1.0f) itemNode.append_child("scaleZ").text().set(data->Scale.z);
					if (data->Rotation.z != 0.0f) itemNode.append_child("roll").text().set(data->Rotation.z);
					if (data->Rotation.x != 0.0f) itemNode.append_child("pitch").text().set(data->Rotation.x);
					if (data->Rotation.y != 0.0f) itemNode.append_child("yaw").text().set(data->Rotation.y);
					if (data->Position.x != 0.0f) itemNode.append_child("x").text().set(data->Position.x);
					if (data->Position.y != 0.0f) itemNode.append_child("y").text().set(data->Position.y);
					if (data->Position.z != 0.0f) itemNode.append_child("z").text().set(data->Position.z);
				}
			}


			// item variable values
			pugi::xml_node itemValuesNode = passNode.append_child("itemvalues");
			std::vector<RenderEngine::ItemVariableValue> itemValues = m_renderer->GetItemVariableValues();
			std::vector<ShaderVariable*>& vars = passData->Variables.GetVariables();
			for (auto itemVal : itemValues) {
				bool found = false;
				for (auto passChild : passData->Items)
					if (passChild == itemVal.Item) {
						found = true;
						break;
					}
				if (!found) continue;

				pugi::xml_node itemValueNode = itemValuesNode.append_child("value");

				itemValueNode.append_attribute("variable").set_value(itemVal.Variable->Name);
				itemValueNode.append_attribute("for").set_value(itemVal.Item->Name);

				m_exportVariableValue(itemValueNode, itemVal.NewValue);
			}
		}

		// objects
		{
			// textures
			std::vector<std::string> texs = m_objects->GetObjects();
			for (int i = 0; i < texs.size(); i++) {
				bool isRT = m_objects->IsRenderTexture(texs[i]);
				bool isAudio = m_objects->IsAudio(texs[i]);
				bool isCube = m_objects->IsCubeMap(texs[i]);
				bool isBuffer = m_objects->IsBuffer(texs[i]);

				pugi::xml_node textureNode = objectsNode.append_child("object");
				textureNode.append_attribute("type").set_value(isBuffer ? "buffer" : (isRT ? "rendertexture" : (isAudio ? "audio" : "texture")));
				textureNode.append_attribute((isRT || isCube || isBuffer) ? "name" : "path").set_value(texs[i].c_str());

				if (!isRT && !isAudio && !isBuffer) {
					bool isCube = m_objects->IsCubeMap(texs[i]);
					if (isCube)
						textureNode.append_attribute("cube").set_value(isCube);
				}

				if (isRT) {
					ed::RenderTextureObject* rtObj = m_objects->GetRenderTexture(m_objects->GetTexture(texs[i]));
					
					if(rtObj->Format != GL_RGBA)
						textureNode.append_attribute("format").set_value(gl::String::Format(rtObj->Format));
					
					if (rtObj->FixedSize.x != -1)
						textureNode.append_attribute("fsize").set_value((std::to_string(rtObj->FixedSize.x) + "," + std::to_string(rtObj->FixedSize.y)).c_str());
					else
						textureNode.append_attribute("rsize").set_value((std::to_string(rtObj->RatioSize.x) + "," + std::to_string(rtObj->RatioSize.y)).c_str());

					textureNode.append_attribute("clear").set_value(rtObj->Clear);
					if (rtObj->ClearColor.r != 0) textureNode.append_attribute("r").set_value(rtObj->ClearColor.r);
					if (rtObj->ClearColor.g != 0) textureNode.append_attribute("g").set_value(rtObj->ClearColor.g);
					if (rtObj->ClearColor.b != 0) textureNode.append_attribute("b").set_value(rtObj->ClearColor.b);
					if (rtObj->ClearColor.a != 0) textureNode.append_attribute("a").set_value(rtObj->ClearColor.a);
				}

				if (isCube) {
					std::vector<std::string> texmaps = m_objects->GetCubemapTextures(texs[i]);

					textureNode.append_attribute("left").set_value(texmaps[0].c_str());
					textureNode.append_attribute("top").set_value(texmaps[1].c_str());
					textureNode.append_attribute("front").set_value(texmaps[2].c_str());
					textureNode.append_attribute("bottom").set_value(texmaps[3].c_str());
					textureNode.append_attribute("right").set_value(texmaps[4].c_str());
					textureNode.append_attribute("back").set_value(texmaps[4].c_str());
				}

				if (isBuffer) {
					ed::BufferObject* bobj = m_objects->GetBuffer(texs[i]);
					
					textureNode.append_attribute("size").set_value(bobj->Size);
					textureNode.append_attribute("format").set_value(bobj->ViewFormat);
					
					std::string bPath = GetProjectPath("buffers/" + texs[i] + ".buf");
					if (!ghc::filesystem::exists(GetProjectPath("buffers")))
						ghc::filesystem::create_directories(GetProjectPath("buffers"));

					std::ofstream bufWrite(bPath, std::ios::binary);
					bufWrite.write((char*)bobj->Data, bobj->Size);
					bufWrite.close();


					for (int j = 0; j < passItems.size(); j++) {
						std::vector<std::string> bound = m_objects->GetUniformBindList(passItems[j]);

						for (int slot = 0; slot < bound.size(); slot++)
							if (bound[slot] == texs[i]) {
								pugi::xml_node bindNode = textureNode.append_child("bind");
								bindNode.append_attribute("slot").set_value(slot);
								bindNode.append_attribute("name").set_value(passItems[j]->Name);
							}
					}
				} else {
					GLuint myTex = m_objects->GetTexture(texs[i]);

					for (int j = 0; j < passItems.size(); j++) {
						std::vector<GLuint> bound = m_objects->GetBindList(passItems[j]);

						for (int slot = 0; slot < bound.size(); slot++)
							if (bound[slot] == myTex) {
								pugi::xml_node bindNode = textureNode.append_child("bind");
								bindNode.append_attribute("slot").set_value(slot);
								bindNode.append_attribute("name").set_value(passItems[j]->Name);
							}
					}
				} 
			}
		}

		// settings
		{
			// property ui
			PropertyUI* props = ((PropertyUI*)m_ui->Get(ViewID::Properties));
			if (props->HasItemSelected()) {
				std::string name = props->CurrentItemName();

				pugi::xml_node propNode = settingsNode.append_child("entry");
				propNode.append_attribute("type").set_value("property");
				propNode.append_attribute("name").set_value(name.c_str());
			}

			// code editor ui
			CodeEditorUI* editor = ((CodeEditorUI*)m_ui->Get(ViewID::Code));
			std::vector<std::pair<std::string, int>> files = editor->GetOpenedFiles();
			for (const auto& file : files) {
				pugi::xml_node fileNode = settingsNode.append_child("entry");
				fileNode.append_attribute("type").set_value("file");
				fileNode.append_attribute("name").set_value(file.first.c_str());
				fileNode.append_attribute("shader").set_value(file.second == 0 ? "vs" : (file.second == 1 ? "ps" : "gs"));
			}

			// pinned ui
			PinnedUI* pinned = ((PinnedUI*)m_ui->Get(ViewID::Pinned));
			std::vector<ShaderVariable*> pinnedVars = pinned->GetAll();
			for (auto var : pinnedVars) {
				pugi::xml_node varNode = settingsNode.append_child("entry");
				varNode.append_attribute("type").set_value("pinned");
				varNode.append_attribute("name").set_value(var->Name);

				for (PipelineItem* passItem : passItems) {
					pipe::ShaderPass* data = (pipe::ShaderPass*)passItem->Data;
					bool found = false;

					std::vector<ShaderVariable*> vars = data->Variables.GetVariables();
					for (int i = 0; i < vars.size(); i++) {
						if (vars[i] == var) {
							varNode.append_attribute("owner").set_value(passItem->Name);
							found = true;
						}
					}

					if (found) break;
				}
			}

			// camera settings
			{
				if (Settings::Instance().Project.FPCamera) {
					ed::FirstPersonCamera* cam = (ed::FirstPersonCamera*)SystemVariableManager::Instance().GetCamera();
					pugi::xml_node camNode = settingsNode.append_child("entry");
					camNode.append_attribute("type").set_value("camera");
					camNode.append_attribute("fp").set_value(true);

					glm::vec3 rota = cam->GetRotation();

					camNode.append_child("positionX").text().set(cam->GetPosition().x);
					camNode.append_child("positionY").text().set(cam->GetPosition().y);
					camNode.append_child("positionZ").text().set(cam->GetPosition().z);
					camNode.append_child("yaw").text().set(rota.x);
					camNode.append_child("pitch").text().set(rota.y);
				} else {
					ed::ArcBallCamera* cam = (ed::ArcBallCamera*)SystemVariableManager::Instance().GetCamera();
					pugi::xml_node camNode = settingsNode.append_child("entry");
					camNode.append_attribute("type").set_value("camera");
					camNode.append_attribute("fp").set_value(false);

					glm::vec3 rota = cam->GetRotation();
					

					camNode.append_child("distance").text().set(cam->GetDistance());
					camNode.append_child("pitch").text().set(rota.x);
					camNode.append_child("yaw").text().set(rota.y);
					camNode.append_child("roll").text().set(rota.z);
				}
			}

			// clear color
			{
				pugi::xml_node colorNode = settingsNode.append_child("entry");
				colorNode.append_attribute("type").set_value("clearcolor");
				colorNode.append_attribute("r").set_value(Settings::Instance().Project.ClearColor.r);
				colorNode.append_attribute("g").set_value(Settings::Instance().Project.ClearColor.g);
				colorNode.append_attribute("b").set_value(Settings::Instance().Project.ClearColor.b);
				colorNode.append_attribute("a").set_value(Settings::Instance().Project.ClearColor.a);
			}
		}

		doc.save_file(file.c_str());
	}
	std::string ProjectParser::LoadProjectFile(const std::string & file)
	{
		std::ifstream in(GetProjectPath(file));
		if (in.is_open()) {
			in.seekg(0, std::ios::beg);

			std::string content((std::istreambuf_iterator<char>(in)), (std::istreambuf_iterator<char>()));
			in.close();
			return content;
		}
		return "";
	}
	char * ProjectParser::LoadProjectFile(const std::string& file, size_t& fsize)
	{
		std::string actual = GetProjectPath(file);

		FILE *f = fopen(actual.c_str(), "rb");
		fseek(f, 0, SEEK_END);
		fsize = ftell(f);
		fseek(f, 0, SEEK_SET);  //same as rewind(f);

		char *string = (char*)malloc(fsize + 1);
		fread(string, fsize, 1, f);
		fclose(f);

		string[fsize] = 0;

		return string;
	}
	eng::Model* ProjectParser::LoadModel(const std::string& file)
	{
		// return already loaded model
		for (auto& mdl : m_models)
			if (mdl.first == file)
				return mdl.second;

		m_models.push_back(std::make_pair(file, new eng::Model()));

		// load the model
		std::string path = GetProjectPath(file);
		bool loaded = m_models[m_models.size() - 1].second->LoadFromFile(path);
		if (!loaded)
			return nullptr;

		return m_models[m_models.size() - 1].second;
	}
	void ProjectParser::SaveProjectFile(const std::string & file, const std::string & data)
	{
		std::ofstream out(GetProjectPath(file));
		out << data;
		out.close();
	}
	std::string ProjectParser::GetRelativePath(const std::string& to)
	{
		ghc::filesystem::path fFrom(m_projectPath);
		ghc::filesystem::path fTo(to);
		
		std::string ret = ghc::filesystem::relative(fTo, fFrom).native();

		return ret;
	}
	std::string ProjectParser::GetProjectPath(const std::string& to)
	{
		return ghc::filesystem::path(m_projectPath + ((m_projectPath[m_projectPath.size() - 1] == '/') ? "" : "/") + to).generic_string();
	}
	bool ProjectParser::FileExists(const std::string& str)
	{
		return ghc::filesystem::exists(GetProjectPath(str));
	}
	void ProjectParser::ResetProjectDirectory()
	{
		m_file = "";
		m_projectPath = ghc::filesystem::current_path().native();
	}


	void ProjectParser::m_parseVariableValue(pugi::xml_node& node, ShaderVariable* var)
	{
		int rowID = 0;
		for (pugi::xml_node row : node.children("row")) {
			int colID = 0;
			for (pugi::xml_node value : row.children("value")) {
				if (var->Function != FunctionShaderVariable::None) {
					if (var->Function == FunctionShaderVariable::Pointer) {
						strcpy(var->Arguments, value.text().as_string());
					} else {
						*FunctionVariableManager::LoadFloat(var->Arguments, colID++) = value.text().as_float();
					}
				} else {
					if (var->GetType() >= ShaderVariable::ValueType::Boolean1 && var->GetType() <= ShaderVariable::ValueType::Boolean4)
						var->SetBooleanValue(value.text().as_bool(), colID++);
					else if (var->GetType() >= ShaderVariable::ValueType::Integer1 && var->GetType() <= ShaderVariable::ValueType::Integer4)
						var->SetIntegerValue(value.text().as_int(), colID++);
					else
						var->SetFloat(value.text().as_float(), colID++, rowID);
				}

			}
			rowID++;
		}
	}
	void ProjectParser::m_exportVariableValue(pugi::xml_node& node, ShaderVariable* var)
	{
		pugi::xml_node valueRowNode = node.append_child("row");

		if (var->Function == FunctionShaderVariable::None) {
			int rowID = 0;
			for (int i = 0; i < ShaderVariable::GetSize(var->GetType()) / 4; i++) {
				if (var->GetType() >= ShaderVariable::ValueType::Boolean1 && var->GetType() <= ShaderVariable::ValueType::Boolean4)
					valueRowNode.append_child("value").text().set(var->AsBoolean(i));
				else if (var->GetType() >= ShaderVariable::ValueType::Integer1 && var->GetType() <= ShaderVariable::ValueType::Integer4)
					valueRowNode.append_child("value").text().set(var->AsInteger(i));
				else
					valueRowNode.append_child("value").text().set(var->AsFloat(i%var->GetColumnCount(), rowID));

				if (i%var->GetColumnCount() == 0 && i != 0) {
					valueRowNode = node.append_child("row");
					rowID++;
				}
			}
		}
		else {
			if (var->Function == FunctionShaderVariable::Pointer) {
				valueRowNode.append_child("value").text().set(var->Arguments);
			} else {
				// save arguments
				for (int i = 0; i < FunctionVariableManager::GetArgumentCount(var->Function); i++) {
					valueRowNode.append_child("value").text().set(*FunctionVariableManager::LoadFloat(var->Arguments, i));
				}
			}
		}
	}
	void ProjectParser::m_exportShaderVariables(pugi::xml_node& node, std::vector<ShaderVariable*>& vars)
	{
		if (vars.size() > 0) {
			pugi::xml_node varsNodes = node.append_child("variables");
			for (auto var : vars) {
				pugi::xml_node varNode = varsNodes.append_child("variable");
				varNode.append_attribute("type").set_value(VARIABLE_TYPE_NAMES[(int)var->GetType()]);
				varNode.append_attribute("name").set_value(var->Name);

				bool isInvert = var->Flags & (char)ShaderVariable::Flag::Inverse;
				bool isLastFrame = var->Flags & (char)ShaderVariable::Flag::LastFrame;
				if (isInvert) varNode.append_attribute("invert").set_value(isInvert);
				if (isLastFrame) varNode.append_attribute("lastframe").set_value(isLastFrame);

				if (var->System != SystemShaderVariable::None)
					varNode.append_attribute("system").set_value(SYSTEM_VARIABLE_NAMES[(int)var->System]);
				else if (var->Function != FunctionShaderVariable::None)
					varNode.append_attribute("function").set_value(FUNCTION_NAMES[(int)var->Function]);

				if (var->System == SystemShaderVariable::None)
					m_exportVariableValue(varNode, var);
			}
		}
	}
	GLenum ProjectParser::m_toBlend(const char* text)
	{
		for (int k = 0; k < HARRAYSIZE(BLEND_NAMES); k++)
			if (strcmp(text, BLEND_NAMES[k]) == 0)
				return BLEND_VALUES[k];
		return GL_CONSTANT_COLOR;
	}
	GLenum ProjectParser::m_toBlendOp(const char* text)
	{
		for (int k = 0; k < HARRAYSIZE(BLEND_OPERATOR_NAMES); k++)
			if (strcmp(text, BLEND_OPERATOR_NAMES[k]) == 0)
				return BLEND_OPERATOR_VALUES[k];
		return GL_FUNC_ADD;
	}
	GLenum ProjectParser::m_toComparisonFunc(const char * str)
	{
		for (int k = 0; k < HARRAYSIZE(COMPARISON_FUNCTION_NAMES); k++)
			if (strcmp(str, COMPARISON_FUNCTION_NAMES[k]) == 0)
				return COMPARISON_FUNCTION_VALUES[k];
		return GL_ALWAYS;
	}
	GLenum ProjectParser::m_toStencilOp(const char * str)
	{
		for (int k = 0; k < HARRAYSIZE(STENCIL_OPERATION_NAMES); k++)
			if (strcmp(str, STENCIL_OPERATION_NAMES[k]) == 0)
				return STENCIL_OPERATION_VALUES[k];
		return GL_KEEP;
	}
	GLenum ProjectParser::m_toCullMode(const char* str)
	{
		for (int k = 0; k < HARRAYSIZE(CULL_MODE_NAMES); k++)
			if (strcmp(str, CULL_MODE_NAMES[k]) == 0)
				return CULL_MODE_VALUES[k];
		return GL_BACK;
	}

	// parser versions
	void ProjectParser::m_parseV1(pugi::xml_node& projectNode)
	{
		Logger::Get().Log("Parsing a V1 project file...");

		std::map<pipe::ShaderPass*, std::vector<std::string>> fbos;

		// shader passes
		for (pugi::xml_node passNode : projectNode.child("pipeline").children("pass")) {
			char name[PIPELINE_ITEM_NAME_LENGTH];
			ed::PipelineItem::ItemType type = ed::PipelineItem::ItemType::ShaderPass;
			ed::pipe::ShaderPass* data = new ed::pipe::ShaderPass();
			
			data->RenderTextures[0] = m_renderer->GetTexture();
			for (int i = 1; i < MAX_RENDER_TEXTURES; i++)
				data->RenderTextures[i] = 0;

			// get pass name
			if (!passNode.attribute("name").empty())
				strcpy(name, passNode.attribute("name").as_string());

			// check if it should be collapsed
			if (!passNode.attribute("collapsed").empty()) {
				bool cs = passNode.attribute("collapsed").as_bool();
				if (cs)
					((PipelineUI*)m_ui->Get(ViewID::Pipeline))->Collapse(data);
			}

			// get render textures
			int rtCur = 0;
			for (pugi::xml_node rtNode : passNode.children("rendertexture")) {
				std::string rtName(rtNode.attribute("name").as_string());
				fbos[data].push_back(rtName);
				rtCur++;
			}
			data->RTCount = (rtCur == 0) ? 1 : rtCur;

			// add the item
			m_pipe->AddPass(name, data);

			// get shader properties (NOTE: a shader must have TYPE, PATH and ENTRY 
			for (pugi::xml_node shaderNode : passNode.children("shader")) {
				std::string shaderNodeType(shaderNode.attribute("type").as_string()); // "vs" or "ps" or "gs"

				// parse path and type
				pugi::char_t shaderPath[MAX_PATH];
				strcpy(shaderPath, toGenericPath(shaderNode.child("path").text().as_string()).c_str());
				const pugi::char_t* shaderEntry = shaderNode.child("entry").text().as_string();
				if (shaderNodeType == "vs") {
					strcpy(data->VSPath, shaderPath);
					strcpy(data->VSEntry, shaderEntry);
				} else if (shaderNodeType == "ps") {
					strcpy(data->PSPath, shaderPath);
					strcpy(data->PSEntry, shaderEntry);
				} else if (shaderNodeType == "gs") {
					if (!shaderNode.attribute("used").empty()) data->GSUsed = shaderNode.attribute("used").as_bool();
					else data->GSUsed = false;
					strcpy(data->GSPath, shaderPath);
					strcpy(data->GSEntry, shaderEntry);
				}

				std::string type = ((shaderNodeType == "vs") ? "vertex" : ((shaderNodeType == "ps") ? "pixel" : "geometry"));
				if (!FileExists(shaderPath))
					m_msgs->Add(ed::MessageStack::Type::Error, name, type + " shader does not exist.");

				
				// parse variables
				for (pugi::xml_node variableNode : shaderNode.child("variables").children("variable")) {
					ShaderVariable::ValueType type = ShaderVariable::ValueType::Float1;
					SystemShaderVariable system = SystemShaderVariable::None;
					FunctionShaderVariable func = FunctionShaderVariable::None;
					
					if (!variableNode.attribute("type").empty()) {
						const char* myType = variableNode.attribute("type").as_string();
						for (int i = 0; i < HARRAYSIZE(VARIABLE_TYPE_NAMES); i++)
							if (strcmp(myType, VARIABLE_TYPE_NAMES[i]) == 0) {
								type = (ed::ShaderVariable::ValueType)i;
								break;
							}
					}
					if (!variableNode.attribute("system").empty()) {
						const char* mySystem = variableNode.attribute("system").as_string();
						for (int i = 0; i < HARRAYSIZE(SYSTEM_VARIABLE_NAMES); i++)
							if (strcmp(mySystem, SYSTEM_VARIABLE_NAMES[i]) == 0) {
								system = (ed::SystemShaderVariable)i;
								break;
							}
						if (SystemVariableManager::GetType(system) != type)
							system = ed::SystemShaderVariable::None;
					}
					if (!variableNode.attribute("function").empty()) {
						const char* myFunc = variableNode.attribute("function").as_string();
						for (int i = 0; i < HARRAYSIZE(FUNCTION_NAMES); i++)
							if (strcmp(myFunc, FUNCTION_NAMES[i]) == 0) {
								func = (FunctionShaderVariable)i;
								break;
							}
						if (system != SystemShaderVariable::None || !FunctionVariableManager::HasValidReturnType(type, func))
							func = FunctionShaderVariable::None;
					}

					ShaderVariable* var = new ShaderVariable(type, variableNode.attribute("name").as_string(), system);
					FunctionVariableManager::AllocateArgumentSpace(var, func);

					// parse value
					if (system == SystemShaderVariable::None)
						m_parseVariableValue(variableNode, var);
						
					data->Variables.Add(var);
				}
			}

			// parse items
			for (pugi::xml_node itemNode : passNode.child("items").children()) {
				char itemName[PIPELINE_ITEM_NAME_LENGTH];
				ed::PipelineItem::ItemType itemType = ed::PipelineItem::ItemType::Geometry;
				void* itemData = nullptr;

				strcpy(itemName, itemNode.attribute("name").as_string());

				// parse the inner content of the item
				if (strcmp(itemNode.attribute("type").as_string(), "geometry") == 0) {
					itemType = ed::PipelineItem::ItemType::Geometry;
					itemData = new pipe::GeometryItem;
					pipe::GeometryItem* tData = (pipe::GeometryItem*)itemData;

					tData->Scale = glm::vec3(1, 1, 1);
					tData->Position = glm::vec3(0, 0, 0);
					tData->Rotation = glm::vec3(0, 0, 0);

					for (pugi::xml_node attrNode : itemNode.children()) {
						if (strcmp(attrNode.name(), "width") == 0)
							tData->Size.x = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "height") == 0)
							tData->Size.y = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "depth") == 0)
							tData->Size.z = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "scaleX") == 0)
							tData->Scale.x = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "scaleY") == 0)
							tData->Scale.y = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "scaleZ") == 0)
							tData->Scale.z = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "roll") == 0)
							tData->Rotation.z = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "yaw") == 0)
							tData->Rotation.y = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "pitch") == 0)
							tData->Rotation.x = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "x") == 0)
							tData->Position.x = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "y") == 0)
							tData->Position.y = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "z") == 0)
							tData->Position.z = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "topology") == 0) {
							for (int k = 0; k < HARRAYSIZE(TOPOLOGY_ITEM_NAMES); k++)
								if (strcmp(attrNode.text().as_string(), TOPOLOGY_ITEM_NAMES[k]) == 0)
									tData->Topology = TOPOLOGY_ITEM_VALUES[k];
						}
						else if (strcmp(attrNode.name(), "type") == 0) {
							for (int k = 0; k < HARRAYSIZE(GEOMETRY_NAMES); k++)
								if (strcmp(attrNode.text().as_string(), GEOMETRY_NAMES[k]) == 0)
									tData->Type = (pipe::GeometryItem::GeometryType)k;
						}
					}
				}
				else if (strcmp(itemNode.attribute("type").as_string(), "blend") == 0) {
					itemType = ed::PipelineItem::ItemType::RenderState;
					itemData = new pipe::RenderState();

					pipe::RenderState* tData = (pipe::RenderState*)itemData;
					glm::vec4 blendFactor(0, 0, 0, 0);

					tData->Blend = true;

					for (pugi::xml_node attrNode : itemNode.children()) {
						if (strcmp(attrNode.name(), "srcblend") == 0)
							tData->BlendSourceFactorRGB = m_toBlend(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "blendop") == 0)
							tData->BlendFunctionColor = m_toBlendOp(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "destblend") == 0)
							tData->BlendDestinationFactorRGB = m_toBlend(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "srcblendalpha") == 0)
							tData->BlendSourceFactorAlpha = m_toBlend(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "alphablendop") == 0)
							tData->BlendFunctionAlpha = m_toBlendOp(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "destblendalpha") == 0)
							tData->BlendDestinationFactorAlpha = m_toBlend(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "alpha2cov") == 0)
							tData->AlphaToCoverage = attrNode.text().as_bool();
						else if (strcmp(attrNode.name(), "bf_red") == 0)
							tData->BlendFactor.x = attrNode.text().as_uint();
						else if (strcmp(attrNode.name(), "bf_green") == 0)
							tData->BlendFactor.y = attrNode.text().as_uint();
						else if (strcmp(attrNode.name(), "bf_blue") == 0)
							tData->BlendFactor.z = attrNode.text().as_uint();
						else if (strcmp(attrNode.name(), "bf_alpha") == 0)
							tData->BlendFactor.w = attrNode.text().as_uint();
					}
				}
				else if (strcmp(itemNode.attribute("type").as_string(), "depthstencil") == 0) {
					itemType = ed::PipelineItem::ItemType::RenderState;
					itemData = new pipe::RenderState;

					pipe::RenderState* tData = (pipe::RenderState*)itemData;
					
					tData->StencilMask = 0xFF;

					for (pugi::xml_node attrNode : itemNode.children()) {
						if (strcmp(attrNode.name(), "depthenable") == 0)
							tData->DepthTest = attrNode.text().as_bool();
						else if (strcmp(attrNode.name(), "depthfunc") == 0)
							tData->DepthFunction = m_toComparisonFunc(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "stencilenable") == 0)
							tData->StencilTest = attrNode.text().as_bool();
						else if (strcmp(attrNode.name(), "frontfunc") == 0)
							tData->StencilFrontFaceFunction = m_toComparisonFunc(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "frontpass") == 0)
							tData->StencilFrontFaceOpPass = m_toStencilOp(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "frontfail") == 0)
							tData->StencilFrontFaceOpStencilFail = tData->StencilFrontFaceOpDepthFail = m_toStencilOp(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "backfunc") == 0)
							tData->StencilBackFaceFunction = m_toComparisonFunc(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "backpass") == 0)
							tData->StencilBackFaceOpPass = m_toStencilOp(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "backfail") == 0)
							tData->StencilBackFaceOpStencilFail = tData->StencilBackFaceOpDepthFail = m_toStencilOp(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "sref") == 0)
							tData->StencilReference = attrNode.text().as_uint();
					}
				}
				else if (strcmp(itemNode.attribute("type").as_string(), "rasterizer") == 0) {
					itemType = ed::PipelineItem::ItemType::RenderState;
					itemData = new pipe::RenderState;

					pipe::RenderState* tData = (pipe::RenderState*)itemData;
					
					for (pugi::xml_node attrNode : itemNode.children()) {
						if (strcmp(attrNode.name(), "wireframe") == 0)
							tData->PolygonMode = attrNode.text().as_bool() ? GL_LINE : GL_FILL;
						else if (strcmp(attrNode.name(), "cull") == 0) {
							tData->CullFaceType = m_toCullMode(attrNode.text().as_string());

							if (tData->CullFaceType == GL_ZERO) tData->CullFace = false;
							else tData->CullFace = true;
						}
						else if (strcmp(attrNode.name(), "ccw") == 0)
							tData->FrontFace = attrNode.text().as_bool() ? GL_CCW : GL_CW;
						else if (strcmp(attrNode.name(), "depthbias") == 0)
							tData->DepthBias = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "depthclip") == 0)
							tData->DepthClamp = attrNode.text().as_bool();
					}
				}
				else if (strcmp(itemNode.attribute("type").as_string(), "model") == 0) {
					itemType = ed::PipelineItem::ItemType::Model;
					itemData = new pipe::Model;

					pipe::Model* mdata = (pipe::Model*)itemData;

					mdata->OnlyGroup = false;
					mdata->Scale = glm::vec3(1, 1, 1);
					mdata->Position = glm::vec3(0, 0, 0);
					mdata->Rotation = glm::vec3(0, 0, 0);

					for (pugi::xml_node attrNode : itemNode.children()) {
						if (strcmp(attrNode.name(), "filepath") == 0)
							strcpy(mdata->Filename, attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "group") == 0)
							strcpy(mdata->GroupName, attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "grouponly") == 0)
							mdata->OnlyGroup = attrNode.text().as_bool();
						else if (strcmp(attrNode.name(), "scaleX") == 0)
							mdata->Scale.x = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "scaleY") == 0)
							mdata->Scale.y = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "scaleZ") == 0)
							mdata->Scale.z = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "roll") == 0)
							mdata->Rotation.z = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "yaw") == 0)
							mdata->Rotation.y = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "pitch") == 0)
							mdata->Rotation.x = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "x") == 0)
							mdata->Position.x = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "y") == 0)
							mdata->Position.y = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "z") == 0)
							mdata->Position.z = attrNode.text().as_float();
					}

					if (strlen(mdata->Filename) > 0)
						strcpy(mdata->Filename, toGenericPath(mdata->Filename).c_str());
				}

				// create and modify if needed
				if (itemType == ed::PipelineItem::ItemType::Geometry) {
					ed::pipe::GeometryItem* tData = reinterpret_cast<ed::pipe::GeometryItem*>(itemData);
					if (tData->Type == pipe::GeometryItem::Cube)
						tData->VAO = eng::GeometryFactory::CreateCube(tData->VBO, tData->Size.x, tData->Size.y, tData->Size.z);
					else if (tData->Type == pipe::GeometryItem::Circle)
						tData->VAO = eng::GeometryFactory::CreateCircle(tData->VBO, tData->Size.x, tData->Size.y);
					else if (tData->Type == pipe::GeometryItem::Plane)
						tData->VAO = eng::GeometryFactory::CreatePlane(tData->VBO, tData->Size.x, tData->Size.y);
					else if (tData->Type == pipe::GeometryItem::Rectangle)
						tData->VAO = eng::GeometryFactory::CreatePlane(tData->VBO, 1, 1);
					else if (tData->Type == pipe::GeometryItem::Sphere)
						tData->VAO = eng::GeometryFactory::CreateSphere(tData->VBO,tData->Size.x);
					else if (tData->Type == pipe::GeometryItem::Triangle)
						tData->VAO = eng::GeometryFactory::CreateTriangle(tData->VBO, tData->Size.x);
				}
				else if (itemType == ed::PipelineItem::ItemType::Model) {
					pipe::Model* tData = reinterpret_cast<pipe::Model*>(itemData);

					//std::string objMem = LoadProjectFile(tData->Filename);
					eng::Model* ptrObject = LoadModel(tData->Filename);
					bool loaded = ptrObject != nullptr;

					if (loaded)
						tData->Data = ptrObject;
					else m_msgs->Add(ed::MessageStack::Type::Error, name, "Failed to load .obj model " + std::string(itemName));
				}

				m_pipe->AddItem(name, itemName, itemType, itemData);
			}

			// parse item values
			for (pugi::xml_node itemValueNode : passNode.child("itemvalues").children("value")) {
				std::string type = itemValueNode.attribute("from").as_string();
				const pugi::char_t* valname = itemValueNode.attribute("variable").as_string();
				const pugi::char_t* itemname = itemValueNode.attribute("for").as_string();

				std::vector<ShaderVariable*> vars = data->Variables.GetVariables();

				ShaderVariable* cpyVar = nullptr;
				for (auto& var : vars)
					if (strcmp(var->Name, valname) == 0) {
						cpyVar = var;
						break;
					}

				if (cpyVar != nullptr) {
					PipelineItem* cpyItem = nullptr;
					for (auto& item : data->Items)
						if (strcmp(item->Name, itemname) == 0) {
							cpyItem = item;
							break;
						}

					RenderEngine::ItemVariableValue ival(cpyVar);
					m_parseVariableValue(itemValueNode, ival.NewValue);
					ival.Item = cpyItem;

					m_renderer->AddItemVariableValue(ival);
				}
			}
		}

		// objects
		std::vector<PipelineItem*> passes = m_pipe->GetList();
		std::map<PipelineItem*, std::vector<std::string>> boundTextures;
		for (pugi::xml_node objectNode : projectNode.child("objects").children("object")) {
			const pugi::char_t* objType = objectNode.attribute("type").as_string();

			if (strcmp(objType, "texture") == 0) {
				pugi::char_t name[MAX_PATH];
				bool isCube = false;
				pugi::char_t cubeLeft[MAX_PATH], cubeRight[MAX_PATH], cubeTop[MAX_PATH],
							cubeBottom[MAX_PATH], cubeFront[MAX_PATH], cubeBack[MAX_PATH];
				if (!objectNode.attribute("cube").empty())
					isCube = objectNode.attribute("cube").as_bool();

				if (isCube) {
					strcpy(name, objectNode.attribute("name").as_string());

					strcpy(cubeLeft, toGenericPath(objectNode.attribute("left").as_string()).c_str());
					strcpy(cubeTop, toGenericPath(objectNode.attribute("top").as_string()).c_str());
					strcpy(cubeFront, toGenericPath(objectNode.attribute("front").as_string()).c_str());
					strcpy(cubeBottom, toGenericPath(objectNode.attribute("bottom").as_string()).c_str());
					strcpy(cubeRight, toGenericPath(objectNode.attribute("right").as_string()).c_str());
					strcpy(cubeBack, toGenericPath(objectNode.attribute("back").as_string()).c_str());
				} else
					strcpy(name, toGenericPath(objectNode.attribute("path").as_string()).c_str());

				for (pugi::xml_node bindNode : objectNode.children("bind")) {
					const pugi::char_t* passBindName = bindNode.attribute("name").as_string();
					int slot = bindNode.attribute("slot").as_int();

					for (auto pass : passes) {
						if (strcmp(pass->Name, passBindName) == 0) {
							if (boundTextures[pass].size() <= slot)
								boundTextures[pass].resize(slot+1);

							boundTextures[pass][slot] = name;
							// TODO: test binding textures that were unbound before opening the project:
							if (isCube)
								m_objects->CreateCubemap(name, cubeLeft, cubeTop, cubeFront, cubeBottom, cubeRight, cubeBack);
							else
								m_objects->CreateTexture(name);
							break;
						}
					}
				}
			}
			else if (strcmp(objType, "rendertexture") == 0) {
				const pugi::char_t* objName = objectNode.attribute("name").as_string();

				m_objects->CreateRenderTexture(objName);
				ed::RenderTextureObject* rt = m_objects->GetRenderTexture(m_objects->GetTexture(objName));
				rt->Format = GL_RGBA;

				// load size
				if (objectNode.attribute("fsize").empty()) { // load RatioSize if attribute fsize (FixedSize) doesnt exist
					std::string rtSize = objectNode.attribute("rsize").as_string();
					float rtSizeX = std::stof(rtSize.substr(0, rtSize.find(',')));
					float rtSizeY = std::stof(rtSize.substr(rtSize.find(',') + 1));

					rt->RatioSize = glm::vec2(rtSizeX, rtSizeY);
					rt->FixedSize = glm::ivec2(-1, -1);

					m_objects->ResizeRenderTexture(objName, rt->CalculateSize(m_renderer->GetLastRenderSize().x, m_renderer->GetLastRenderSize().y));
				} else {
					std::string rtSize = objectNode.attribute("fsize").as_string();
					int rtSizeX = std::stoi(rtSize.substr(0, rtSize.find(',')));
					int rtSizeY = std::stoi(rtSize.substr(rtSize.find(',') + 1));

					rt->FixedSize = glm::ivec2(rtSizeX, rtSizeY);

					m_objects->ResizeRenderTexture(objName, rt->FixedSize);
				}

				// load clear color
				rt->Clear = true;
				if (!objectNode.attribute("r").empty()) rt->ClearColor.r = objectNode.attribute("r").as_int() / 255.0f;
				else rt->ClearColor.r = 0;
				if (!objectNode.attribute("g").empty()) rt->ClearColor.g = objectNode.attribute("g").as_int() / 255.0f;
				else rt->ClearColor.g = 0;
				if (!objectNode.attribute("b").empty()) rt->ClearColor.b = objectNode.attribute("b").as_int() / 255.0f;
				else rt->ClearColor.b = 0;
				if (!objectNode.attribute("a").empty()) rt->ClearColor.a = objectNode.attribute("a").as_int() / 255.0f;
				else rt->ClearColor.a = 0;

				// load binds
				for (pugi::xml_node bindNode : objectNode.children("bind")) {
					const pugi::char_t* passBindName = bindNode.attribute("name").as_string();
					int slot = bindNode.attribute("slot").as_int();

					for (auto pass : passes) {
						if (strcmp(pass->Name, passBindName) == 0) {
							if (boundTextures[pass].size() <= slot)
								boundTextures[pass].resize(slot + 1);

							boundTextures[pass][slot] = objName;
							break;
						}
					}
				}
			}
			else if (strcmp(objType, "audio") == 0) {
				pugi::char_t objPath[MAX_PATH];
				strcpy(objPath, toGenericPath(objectNode.attribute("path").as_string()).c_str());

				m_objects->CreateAudio(std::string(objPath));

				for (pugi::xml_node bindNode : objectNode.children("bind")) {
					const pugi::char_t* passBindName = bindNode.attribute("name").as_string();
					int slot = bindNode.attribute("slot").as_int();

					for (auto pass : passes) {
						if (strcmp(pass->Name, passBindName) == 0) {
							if (boundTextures[pass].size() <= slot)
								boundTextures[pass].resize(slot + 1);

							boundTextures[pass][slot] = objPath;
							break;
						}
					}
				}
			}
		}
		
		// bind objects
		for (auto b : boundTextures)
			for (auto id : b.second)
				if (!id.empty())
					m_objects->Bind(id, b.first);

		// settings
		for (pugi::xml_node settingItem : projectNode.child("settings").children("entry")) {
			if (!settingItem.attribute("type").empty()) {
				std::string type = settingItem.attribute("type").as_string();
				if (type == "property") {
					PropertyUI* props = ((PropertyUI*)m_ui->Get(ViewID::Properties));
					if (!settingItem.attribute("name").empty()) {
						PipelineItem* item = m_pipe->Get(settingItem.attribute("name").as_string());
						props->Open(item);
					}
				}
				else if (type == "file" && Settings::Instance().General.ReopenShaders) {
					CodeEditorUI* editor = ((CodeEditorUI*)m_ui->Get(ViewID::Code));
					if (!settingItem.attribute("name").empty()) {
						PipelineItem* item = m_pipe->Get(settingItem.attribute("name").as_string());
						const pugi::char_t* shaderType = settingItem.attribute("shader").as_string();

						std::string type = ((strcmp(shaderType, "vs") == 0) ? "vertex" : ((strcmp(shaderType, "ps") == 0) ? "pixel" : "geometry"));
						std::string path = ((ed::pipe::ShaderPass*)item->Data)->VSPath;
						
						if (strcmp(shaderType, "ps") == 0)
							path = ((ed::pipe::ShaderPass*)item->Data)->PSPath;
						else if (strcmp(shaderType, "gs") == 0)
							path = ((ed::pipe::ShaderPass*)item->Data)->GSPath;

						if (strcmp(shaderType, "vs") == 0 && FileExists(path))
							editor->OpenVS(*item);
						else if (strcmp(shaderType, "ps") == 0 && FileExists(path))
							editor->OpenPS(*item);
						else if (strcmp(shaderType, "gs") == 0 && FileExists(path))
							editor->OpenGS(*item);
					}
				}
				else if (type == "pinned") {
					PinnedUI* pinned = ((PinnedUI*)m_ui->Get(ViewID::Pinned));
					if (!settingItem.attribute("name").empty()) {
						const pugi::char_t* item = settingItem.attribute("name").as_string();
						const pugi::char_t* shaderType = settingItem.attribute("from").as_string();
						pipe::ShaderPass* owner = (pipe::ShaderPass*)(m_pipe->Get(settingItem.attribute("owner").as_string())->Data);

						std::vector<ShaderVariable*> vars = owner->Variables.GetVariables();

						for (auto var : vars)
							if (strcmp(var->Name, item) == 0) {
								pinned->Add(var);
								break;
							}
					}
				}
				else if (type == "camera") {
					if (settingItem.attribute("fp").empty())
						Settings::Instance().Project.FPCamera = false;
					else
						Settings::Instance().Project.FPCamera = settingItem.attribute("fp").as_bool();

					SystemVariableManager::Instance().GetCamera()->Reset();

					bool fp = Settings::Instance().Project.FPCamera;

					if (fp) {
						ed::FirstPersonCamera* fpCam = (ed::FirstPersonCamera*)SystemVariableManager::Instance().GetCamera();
						fpCam->Reset();
						fpCam->SetPosition(std::stof(settingItem.child("positionX").text().get()),
									   std::stof(settingItem.child("positionY").text().get()),
									   std::stof(settingItem.child("positionZ").text().get())
						);
						fpCam->SetYaw(std::stof(settingItem.child("yaw").text().get()));
						fpCam->SetPitch(std::stof(settingItem.child("pitch").text().get()));
					} else {
						ed::ArcBallCamera* ab = (ed::ArcBallCamera*)SystemVariableManager::Instance().GetCamera();
						ab->SetDistance(std::stof(settingItem.child("distance").text().get()));
						ab->SetYaw(std::stof(settingItem.child("rotationX").text().get()));
						ab->SetPitch(std::stof(settingItem.child("rotationY").text().get()));
						ab->SetRoll(std::stof(settingItem.child("rotationZ").text().get()));
					}

				}
				else if (type == "clearcolor") {
					if (!settingItem.attribute("r").empty())
						Settings::Instance().Project.ClearColor.r = settingItem.attribute("r").as_uint() / 255.0f;
					if (!settingItem.attribute("g").empty())
						Settings::Instance().Project.ClearColor.g = settingItem.attribute("g").as_uint() / 255.0f;
					if (!settingItem.attribute("b").empty())
						Settings::Instance().Project.ClearColor.b = settingItem.attribute("b").as_uint() / 255.0f;
					if (!settingItem.attribute("a").empty())
						Settings::Instance().Project.ClearColor.a = settingItem.attribute("a").as_uint() / 255.0f;
				}
			}
		}
	
		// set actual render texture IDs
		for (auto& pass : fbos) {
			int index = 0;
			for (auto& rtName : pass.second) {
				GLuint rtID = (rtName == "Window") ? m_renderer->GetTexture() : m_objects->GetTexture(rtName);
				pass.first->RenderTextures[index] = rtID;
				index++;
			}
		}
	}
	void ProjectParser::m_parseV2(pugi::xml_node& projectNode)
	{
		Logger::Get().Log("Parsing a V2 project file...");

		std::map<pipe::ShaderPass*, std::vector<std::string>> fbos;

		// shader passes
		for (pugi::xml_node passNode : projectNode.child("pipeline").children("pass")) {
			char name[PIPELINE_ITEM_NAME_LENGTH];
			ed::PipelineItem::ItemType type = ed::PipelineItem::ItemType::ShaderPass;
			ed::pipe::ShaderPass* data = new ed::pipe::ShaderPass();

			data->RenderTextures[0] = m_renderer->GetTexture();
			for (int i = 1; i < MAX_RENDER_TEXTURES; i++)
				data->RenderTextures[i] = 0;

			// get pass name
			if (!passNode.attribute("name").empty())
				strcpy(name, passNode.attribute("name").as_string());

			// check if it should be collapsed
			if (!passNode.attribute("collapsed").empty()) {
				bool cs = passNode.attribute("collapsed").as_bool();
				if (cs)
					((PipelineUI*)m_ui->Get(ViewID::Pipeline))->Collapse(data);
			}

			// get render textures
			int rtCur = 0;
			for (pugi::xml_node rtNode : passNode.children("rendertexture")) {
				std::string rtName("");
				if (!rtNode.attribute("name").empty())
					rtName = std::string(rtNode.attribute("name").as_string());
				fbos[data].push_back(rtName);
				rtCur++;
			}
			data->RTCount = (rtCur == 0) ? 1 : rtCur;

			// add the item
			m_pipe->AddPass(name, data);

			// get shader properties (NOTE: a shader must have TYPE, PATH and ENTRY)
			for (pugi::xml_node shaderNode : passNode.children("shader")) {
				std::string shaderNodeType(shaderNode.attribute("type").as_string()); // "vs" or "ps" or "gs"

				// parse path and type
				pugi::char_t shaderPath[MAX_PATH];
				strcpy(shaderPath, toGenericPath(shaderNode.attribute("path").as_string()).c_str());
				const pugi::char_t* shaderEntry = shaderNode.attribute("entry").as_string();
				
				if (shaderNodeType == "vs") {
					strcpy(data->VSPath, shaderPath);
					strcpy(data->VSEntry, shaderEntry);
				}
				else if (shaderNodeType == "ps") {
					strcpy(data->PSPath, shaderPath);
					strcpy(data->PSEntry, shaderEntry);
				}
				else if (shaderNodeType == "gs") {
					if (!shaderNode.attribute("used").empty()) data->GSUsed = shaderNode.attribute("used").as_bool();
					else data->GSUsed = false;
					strcpy(data->GSPath, shaderPath);
					strcpy(data->GSEntry, shaderEntry);
				}

				std::string type = ((shaderNodeType == "vs") ? "vertex" : ((shaderNodeType == "ps") ? "pixel" : "geometry"));
				if (!FileExists(shaderPath))
					m_msgs->Add(ed::MessageStack::Type::Error, name, type + " shader does not exist.");
			}

			// parse variables
			for (pugi::xml_node variableNode : passNode.child("variables").children("variable")) {
				ShaderVariable::ValueType type = ShaderVariable::ValueType::Float1;
				SystemShaderVariable system = SystemShaderVariable::None;
				FunctionShaderVariable func = FunctionShaderVariable::None;
				char flags = 0;

				/* FLAGS */
				bool isInvert = false, isLastFrame = false;

				if (!variableNode.attribute("invert").empty())
					isInvert = variableNode.attribute("invert").as_bool();
				if (!variableNode.attribute("lastframe").empty())
					isLastFrame = variableNode.attribute("lastframe").as_bool();

				flags = (isInvert * (char)ShaderVariable::Flag::Inverse) |
						(isLastFrame * (char)ShaderVariable::Flag::LastFrame);
						
				/* TYPE */
				if (!variableNode.attribute("type").empty()) {
					const char* myType = variableNode.attribute("type").as_string();
					for (int i = 0; i < HARRAYSIZE(VARIABLE_TYPE_NAMES); i++)
						if (strcmp(myType, VARIABLE_TYPE_NAMES[i]) == 0) {
							type = (ed::ShaderVariable::ValueType)i;
							break;
						}
				}
				if (!variableNode.attribute("system").empty()) {
					const char* mySystem = variableNode.attribute("system").as_string();
					for (int i = 0; i < HARRAYSIZE(SYSTEM_VARIABLE_NAMES); i++)
						if (strcmp(mySystem, SYSTEM_VARIABLE_NAMES[i]) == 0) {
							system = (ed::SystemShaderVariable)i;
							break;
						}
					if (SystemVariableManager::GetType(system) != type)
						system = ed::SystemShaderVariable::None;
				}
				if (!variableNode.attribute("function").empty()) {
					const char* myFunc = variableNode.attribute("function").as_string();
					for (int i = 0; i < HARRAYSIZE(FUNCTION_NAMES); i++)
						if (strcmp(myFunc, FUNCTION_NAMES[i]) == 0) {
							func = (FunctionShaderVariable)i;
							break;
						}
					if (system != SystemShaderVariable::None || !FunctionVariableManager::HasValidReturnType(type, func))
						func = FunctionShaderVariable::None;
				}

				ShaderVariable* var = new ShaderVariable(type, variableNode.attribute("name").as_string(), system);
				var->Flags = flags;
				FunctionVariableManager::AllocateArgumentSpace(var, func);

				// parse value
				if (system == SystemShaderVariable::None)
					m_parseVariableValue(variableNode, var);

				data->Variables.Add(var);
			}

			// macros
			pugi::xml_node macrosNode = passNode.append_child("macros");		
			for (pugi::xml_node macroNode : passNode.child("macros").children("define")) {
				ShaderMacro newMacro;
				if (!macroNode.attribute("name").empty())
					strcpy(newMacro.Name, macroNode.attribute("name").as_string());
				
				newMacro.Active = true;
				if (!macroNode.attribute("active").empty())
					newMacro.Active = macroNode.attribute("active").as_bool();
				strcpy(newMacro.Value, macroNode.text().get());
				data->Macros.push_back(newMacro);
			}

			// parse items
			for (pugi::xml_node itemNode : passNode.child("items").children()) {
				char itemName[PIPELINE_ITEM_NAME_LENGTH];
				ed::PipelineItem::ItemType itemType = ed::PipelineItem::ItemType::Geometry;
				void* itemData = nullptr;

				strcpy(itemName, itemNode.attribute("name").as_string());

				// parse the inner content of the item
				if (strcmp(itemNode.attribute("type").as_string(), "geometry") == 0) {
					itemType = ed::PipelineItem::ItemType::Geometry;
					itemData = new pipe::GeometryItem;
					pipe::GeometryItem* tData = (pipe::GeometryItem*)itemData;

					tData->Scale = glm::vec3(1, 1, 1);
					tData->Position = glm::vec3(0, 0, 0);
					tData->Rotation = glm::vec3(0, 0, 0);

					for (pugi::xml_node attrNode : itemNode.children()) {
						if (strcmp(attrNode.name(), "width") == 0)
							tData->Size.x = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "height") == 0)
							tData->Size.y = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "depth") == 0)
							tData->Size.z = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "scaleX") == 0)
							tData->Scale.x = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "scaleY") == 0)
							tData->Scale.y = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "scaleZ") == 0)
							tData->Scale.z = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "roll") == 0)
							tData->Rotation.z = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "yaw") == 0)
							tData->Rotation.y = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "pitch") == 0)
							tData->Rotation.x = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "x") == 0)
							tData->Position.x = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "y") == 0)
							tData->Position.y = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "z") == 0)
							tData->Position.z = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "topology") == 0) {
							for (int k = 0; k < HARRAYSIZE(TOPOLOGY_ITEM_NAMES); k++)
								if (strcmp(attrNode.text().as_string(), TOPOLOGY_ITEM_NAMES[k]) == 0)
									tData->Topology = TOPOLOGY_ITEM_VALUES[k];
						}
						else if (strcmp(attrNode.name(), "type") == 0) {
							for (int k = 0; k < HARRAYSIZE(GEOMETRY_NAMES); k++)
								if (strcmp(attrNode.text().as_string(), GEOMETRY_NAMES[k]) == 0)
									tData->Type = (pipe::GeometryItem::GeometryType)k;
						}
					}
				}
				else if (strcmp(itemNode.attribute("type").as_string(), "renderstate") == 0) {
					itemType = ed::PipelineItem::ItemType::RenderState;
					itemData = new pipe::RenderState;

					pipe::RenderState* tData = (pipe::RenderState*)itemData;

					for (pugi::xml_node attrNode : itemNode.children()) {
						// rasterizer
						if (strcmp(attrNode.name(), "wireframe") == 0)
							tData->PolygonMode = attrNode.text().as_bool() ? GL_LINE : GL_FILL;
						else if (strcmp(attrNode.name(), "cull") == 0)
							tData->CullFace = attrNode.text().as_bool();
						else if (strcmp(attrNode.name(), "cullfront") == 0)
							tData->CullFaceType = attrNode.text().as_bool() ? GL_FRONT : GL_BACK;
						else if (strcmp(attrNode.name(), "ccw") == 0)
							tData->FrontFace = attrNode.text().as_bool() ? GL_CCW : GL_CW;

						// blend
						else if (strcmp(attrNode.name(), "blend") == 0)
							tData->Blend = attrNode.text().as_bool();
						else if (strcmp(attrNode.name(), "colorsrcfactor") == 0)
							tData->BlendSourceFactorRGB = m_toBlend(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "colorfunc") == 0)
							tData->BlendFunctionColor = m_toBlendOp(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "colordstfactor") == 0)
							tData->BlendDestinationFactorRGB = m_toBlend(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "alphasrcfactor") == 0)
							tData->BlendSourceFactorAlpha = m_toBlend(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "alphafunc") == 0)
							tData->BlendFunctionAlpha = m_toBlendOp(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "alphadstfactor") == 0)
							tData->BlendDestinationFactorAlpha = m_toBlend(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "alpha2coverage") == 0)
							tData->AlphaToCoverage = attrNode.text().as_bool();
						else if (strcmp(attrNode.name(), "blendfactor_r") == 0)
							tData->BlendFactor.r = attrNode.text().as_uint();
						else if (strcmp(attrNode.name(), "blendfactor_g") == 0)
							tData->BlendFactor.g = attrNode.text().as_uint();
						else if (strcmp(attrNode.name(), "blendfactor_b") == 0)
							tData->BlendFactor.b = attrNode.text().as_uint();
						else if (strcmp(attrNode.name(), "blendfactor_a") == 0)
							tData->BlendFactor.a = attrNode.text().as_uint();

						// depth
						else if (strcmp(attrNode.name(), "depthtest") == 0)
							tData->DepthTest = attrNode.text().as_bool();
						else if (strcmp(attrNode.name(), "depthfunc") == 0)
							tData->DepthFunction = m_toComparisonFunc(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "depthbias") == 0)
							tData->DepthBias = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "depthclamp") == 0)
							tData->DepthClamp = attrNode.text().as_bool();
						else if (strcmp(attrNode.name(), "depthmask") == 0)
							tData->DepthMask = attrNode.text().as_bool();

						// stencil
						else if (strcmp(attrNode.name(), "stenciltest") == 0)
							tData->StencilTest = attrNode.text().as_bool();
						else if (strcmp(attrNode.name(), "stencilmask") == 0)
							tData->StencilMask = attrNode.text().as_uint();
						else if (strcmp(attrNode.name(), "stencilref") == 0)
							tData->StencilReference = attrNode.text().as_uint();
						else if (strcmp(attrNode.name(), "stencilfrontfunc") == 0)
							tData->StencilFrontFaceFunction = m_toComparisonFunc(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "stencilfrontpass") == 0)
							tData->StencilFrontFaceOpPass = m_toStencilOp(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "stencilfrontfail") == 0)
							tData->StencilFrontFaceOpStencilFail = m_toStencilOp(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "stencilbackfunc") == 0)
							tData->StencilBackFaceFunction = m_toComparisonFunc(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "stencilbackpass") == 0)
							tData->StencilBackFaceOpPass = m_toStencilOp(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "stencilbackfail") == 0)
							tData->StencilBackFaceOpStencilFail = m_toStencilOp(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "depthfrontfail") == 0)
							tData->StencilFrontFaceOpDepthFail = m_toStencilOp(attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "depthbackfail") == 0)
							tData->StencilBackFaceOpDepthFail = m_toStencilOp(attrNode.text().as_string());
					}
				}
				else if (strcmp(itemNode.attribute("type").as_string(), "model") == 0) {
					itemType = ed::PipelineItem::ItemType::Model;
					itemData = new pipe::Model;

					pipe::Model* mdata = (pipe::Model*)itemData;

					mdata->OnlyGroup = false;
					mdata->Scale = glm::vec3(1, 1, 1);
					mdata->Position = glm::vec3(0, 0, 0);
					mdata->Rotation = glm::vec3(0, 0, 0);

					for (pugi::xml_node attrNode : itemNode.children()) {
						if (strcmp(attrNode.name(), "filepath") == 0)
							strcpy(mdata->Filename, attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "group") == 0)
							strcpy(mdata->GroupName, attrNode.text().as_string());
						else if (strcmp(attrNode.name(), "grouponly") == 0)
							mdata->OnlyGroup = attrNode.text().as_bool();
						else if (strcmp(attrNode.name(), "scaleX") == 0)
							mdata->Scale.x = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "scaleY") == 0)
							mdata->Scale.y = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "scaleZ") == 0)
							mdata->Scale.z = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "roll") == 0)
							mdata->Rotation.z = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "yaw") == 0)
							mdata->Rotation.y = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "pitch") == 0)
							mdata->Rotation.x = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "x") == 0)
							mdata->Position.x = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "y") == 0)
							mdata->Position.y = attrNode.text().as_float();
						else if (strcmp(attrNode.name(), "z") == 0)
							mdata->Position.z = attrNode.text().as_float();
					}

					if (strlen(mdata->Filename) > 0)
						strcpy(mdata->Filename, toGenericPath(mdata->Filename).c_str());
				}

				// create and modify if needed
				if (itemType == ed::PipelineItem::ItemType::Geometry) {
					ed::pipe::GeometryItem* tData = reinterpret_cast<ed::pipe::GeometryItem*>(itemData);
					if (tData->Type == pipe::GeometryItem::Cube)
						tData->VAO = eng::GeometryFactory::CreateCube(tData->VBO, tData->Size.x, tData->Size.y, tData->Size.z);
					else if (tData->Type == pipe::GeometryItem::Circle)
						tData->VAO = eng::GeometryFactory::CreateCircle(tData->VBO, tData->Size.x, tData->Size.y);
					else if (tData->Type == pipe::GeometryItem::Plane)
						tData->VAO = eng::GeometryFactory::CreatePlane(tData->VBO, tData->Size.x, tData->Size.y);
					else if (tData->Type == pipe::GeometryItem::Rectangle)
						tData->VAO = eng::GeometryFactory::CreatePlane(tData->VBO, 1, 1);
					else if (tData->Type == pipe::GeometryItem::Sphere)
						tData->VAO = eng::GeometryFactory::CreateSphere(tData->VBO, tData->Size.x);
					else if (tData->Type == pipe::GeometryItem::Triangle)
						tData->VAO = eng::GeometryFactory::CreateTriangle(tData->VBO, tData->Size.x);
				}
				else if (itemType == ed::PipelineItem::ItemType::Model) {
					pipe::Model* tData = reinterpret_cast<pipe::Model*>(itemData);

					//std::string objMem = LoadProjectFile(tData->Filename);
					eng::Model* ptrObject = LoadModel(tData->Filename);
					bool loaded = ptrObject != nullptr;

					if (loaded)
						tData->Data = ptrObject;
					else m_msgs->Add(ed::MessageStack::Type::Error, name, "Failed to load .obj model " + std::string(itemName));
				}

				m_pipe->AddItem(name, itemName, itemType, itemData);
			}

			// parse item values
			for (pugi::xml_node itemValueNode : passNode.child("itemvalues").children("value")) {
				std::string type = itemValueNode.attribute("from").as_string();
				const pugi::char_t* valname = itemValueNode.attribute("variable").as_string();
				const pugi::char_t* itemname = itemValueNode.attribute("for").as_string();

				std::vector<ShaderVariable*> vars = data->Variables.GetVariables();

				ShaderVariable* cpyVar = nullptr;
				for (auto& var : vars)
					if (strcmp(var->Name, valname) == 0) {
						cpyVar = var;
						break;
					}

				if (cpyVar != nullptr) {
					PipelineItem* cpyItem = nullptr;
					for (auto& item : data->Items)
						if (strcmp(item->Name, itemname) == 0) {
							cpyItem = item;
							break;
						}

					RenderEngine::ItemVariableValue ival(cpyVar);
					m_parseVariableValue(itemValueNode, ival.NewValue);
					ival.Item = cpyItem;

					m_renderer->AddItemVariableValue(ival);
				}
			}
		}

		// objects
		std::vector<PipelineItem*> passes = m_pipe->GetList();
		std::map<PipelineItem*, std::vector<std::string>> boundTextures, boundUBOs;
		for (pugi::xml_node objectNode : projectNode.child("objects").children("object")) {
			const pugi::char_t* objType = objectNode.attribute("type").as_string();

			if (strcmp(objType, "texture") == 0) {
				pugi::char_t name[MAX_PATH];
				bool isCube = false;
				pugi::char_t cubeLeft[MAX_PATH], cubeRight[MAX_PATH], cubeTop[MAX_PATH],
							cubeBottom[MAX_PATH], cubeFront[MAX_PATH], cubeBack[MAX_PATH];
				if (!objectNode.attribute("cube").empty())
					isCube = objectNode.attribute("cube").as_bool();

				if (isCube) {
					strcpy(name, objectNode.attribute("name").as_string());

					strcpy(cubeLeft, toGenericPath(objectNode.attribute("left").as_string()).c_str());
					strcpy(cubeTop, toGenericPath(objectNode.attribute("top").as_string()).c_str());
					strcpy(cubeFront, toGenericPath(objectNode.attribute("front").as_string()).c_str());
					strcpy(cubeBottom, toGenericPath(objectNode.attribute("bottom").as_string()).c_str());
					strcpy(cubeRight, toGenericPath(objectNode.attribute("right").as_string()).c_str());
					strcpy(cubeBack, toGenericPath(objectNode.attribute("back").as_string()).c_str());
				}
				else
					strcpy(name, toGenericPath(objectNode.attribute("path").as_string()).c_str());
				
				for (pugi::xml_node bindNode : objectNode.children("bind")) {
					const pugi::char_t* passBindName = bindNode.attribute("name").as_string();
					int slot = bindNode.attribute("slot").as_int();

					for (auto pass : passes) {
						if (strcmp(pass->Name, passBindName) == 0) {
							if (boundTextures[pass].size() <= slot)
								boundTextures[pass].resize(slot + 1);

							boundTextures[pass][slot] = name;
							// TODO: test binding textures that were unbound before opening the project:
							if (isCube)
								m_objects->CreateCubemap(name, cubeLeft, cubeTop, cubeFront, cubeBottom, cubeRight, cubeBack);
							else
								m_objects->CreateTexture(name);
							break;
						}
					}
				}
			}
			else if (strcmp(objType, "rendertexture") == 0) {
				const pugi::char_t* objName = objectNode.attribute("name").as_string();

				m_objects->CreateRenderTexture(objName);
				ed::RenderTextureObject* rt = m_objects->GetRenderTexture(m_objects->GetTexture(objName));

				// load format
				if (!objectNode.attribute("format").empty()) {
					auto formatName = objectNode.attribute("format").as_string();
					for (int i = 0; i < HARRAYSIZE(FORMAT_NAMES); i++) {
						if (strcmp(formatName, FORMAT_NAMES[i]) == 0) {
							rt->Format = FORMAT_VALUES[i];
							break;
						}
					}
				}

				// load size
				if (objectNode.attribute("fsize").empty()) { // load RatioSize if attribute fsize (FixedSize) doesnt exist
					std::string rtSize = objectNode.attribute("rsize").as_string();
					float rtSizeX = std::stof(rtSize.substr(0, rtSize.find(',')));
					float rtSizeY = std::stof(rtSize.substr(rtSize.find(',') + 1));

					rt->RatioSize = glm::vec2(rtSizeX, rtSizeY);
					rt->FixedSize = glm::ivec2(-1, -1);

					m_objects->ResizeRenderTexture(objName, rt->CalculateSize(m_renderer->GetLastRenderSize().x, m_renderer->GetLastRenderSize().y));
				}
				else {
					std::string rtSize = objectNode.attribute("fsize").as_string();
					int rtSizeX = std::stoi(rtSize.substr(0, rtSize.find(',')));
					int rtSizeY = std::stoi(rtSize.substr(rtSize.find(',') + 1));

					rt->FixedSize = glm::ivec2(rtSizeX, rtSizeY);

					m_objects->ResizeRenderTexture(objName, rt->FixedSize);
				}

				// load clear flag
				rt->Clear = true;
				if (!objectNode.attribute("clear").empty())
					rt->Clear = objectNode.attribute("clear").as_bool();

				// load clear color
				if (!objectNode.attribute("r").empty()) rt->ClearColor.r = objectNode.attribute("r").as_float();
				else rt->ClearColor.r = 0;
				if (!objectNode.attribute("g").empty()) rt->ClearColor.g = objectNode.attribute("g").as_float();
				else rt->ClearColor.g = 0;
				if (!objectNode.attribute("b").empty()) rt->ClearColor.b = objectNode.attribute("b").as_float();
				else rt->ClearColor.b = 0;
				if (!objectNode.attribute("a").empty()) rt->ClearColor.a = objectNode.attribute("a").as_float();
				else rt->ClearColor.a = 0;

				// load binds
				for (pugi::xml_node bindNode : objectNode.children("bind")) {
					const pugi::char_t* passBindName = bindNode.attribute("name").as_string();
					int slot = bindNode.attribute("slot").as_int();

					for (auto pass : passes) {
						if (strcmp(pass->Name, passBindName) == 0) {
							if (boundTextures[pass].size() <= slot)
								boundTextures[pass].resize(slot + 1);

							boundTextures[pass][slot] = objName;
							break;
						}
					}
				}
			}
			else if (strcmp(objType, "audio") == 0) {
				pugi::char_t objPath[MAX_PATH];
				strcpy(objPath, toGenericPath(objectNode.attribute("path").as_string()).c_str());

				m_objects->CreateAudio(std::string(objPath));

				for (pugi::xml_node bindNode : objectNode.children("bind")) {
					const pugi::char_t* passBindName = bindNode.attribute("name").as_string();
					int slot = bindNode.attribute("slot").as_int();

					for (auto pass : passes) {
						if (strcmp(pass->Name, passBindName) == 0) {
							if (boundTextures[pass].size() <= slot)
								boundTextures[pass].resize(slot + 1);

							boundTextures[pass][slot] = objPath;
							break;
						}
					}
				}
			}
			else if (strcmp(objType, "buffer") == 0) {
				const pugi::char_t* objName = objectNode.attribute("name").as_string();
				
				m_objects->CreateBuffer(objName);
				ed::BufferObject* buf = m_objects->GetBuffer(objName);

				if (!objectNode.attribute("size").empty()) {
					buf->Size = objectNode.attribute("size").as_int();
					buf->Data = realloc(buf->Data, buf->Size);
				}
				if (!objectNode.attribute("format").empty())
					strcpy(buf->ViewFormat, objectNode.attribute("format").as_string());
				
				std::string bPath = GetProjectPath("buffers/" + std::string(objName) + ".buf");
				std::ifstream bufRead(bPath, std::ios::binary);
				if (bufRead.is_open())
					bufRead.read((char*)buf->Data, buf->Size);
				bufRead.close();

				glBindBuffer(GL_UNIFORM_BUFFER, buf->ID);
				glBufferData(GL_UNIFORM_BUFFER, buf->Size, buf->Data, GL_STATIC_DRAW); // allocate 0 bytes of memory
				glBindBuffer(GL_UNIFORM_BUFFER, 0);
				
				for (pugi::xml_node bindNode : objectNode.children("bind")) {
					const pugi::char_t* passBindName = bindNode.attribute("name").as_string();
					int slot = bindNode.attribute("slot").as_int();

					for (auto pass : passes) {
						if (strcmp(pass->Name, passBindName) == 0) {
							if (boundUBOs[pass].size() <= slot)
								boundUBOs[pass].resize(slot + 1);

							boundUBOs[pass][slot] = objName;
							break;
						}
					}
				}
			}
		}

		// bind objects
		for (auto b : boundTextures)
			for (auto id : b.second)
				if (!id.empty())
					m_objects->Bind(id, b.first);
		// bind buffers
		for (auto b : boundUBOs)
			for (auto id : b.second)
				if (!id.empty())
					m_objects->BindUniform(id, b.first);


		// settings
		for (pugi::xml_node settingItem : projectNode.child("settings").children("entry")) {
			if (!settingItem.attribute("type").empty()) {
				std::string type = settingItem.attribute("type").as_string();
				if (type == "property") {
					PropertyUI* props = ((PropertyUI*)m_ui->Get(ViewID::Properties));
					if (!settingItem.attribute("name").empty()) {
						PipelineItem* item = m_pipe->Get(settingItem.attribute("name").as_string());
						props->Open(item);
					}
				}
				else if (type == "file" && Settings::Instance().General.ReopenShaders) {
					CodeEditorUI* editor = ((CodeEditorUI*)m_ui->Get(ViewID::Code));
					if (!settingItem.attribute("name").empty()) {
						PipelineItem* item = m_pipe->Get(settingItem.attribute("name").as_string());
						const pugi::char_t* shaderType = settingItem.attribute("shader").as_string();

						std::string type = ((strcmp(shaderType, "vs") == 0) ? "vertex" : ((strcmp(shaderType, "ps") == 0) ? "pixel" : "geometry"));
						std::string path = ((ed::pipe::ShaderPass*)item->Data)->VSPath;

						if (strcmp(shaderType, "ps") == 0)
							path = ((ed::pipe::ShaderPass*)item->Data)->PSPath;
						else if (strcmp(shaderType, "gs") == 0)
							path = ((ed::pipe::ShaderPass*)item->Data)->GSPath;

						if (strcmp(shaderType, "vs") == 0 && FileExists(path))
							editor->OpenVS(*item);
						else if (strcmp(shaderType, "ps") == 0 && FileExists(path))
							editor->OpenPS(*item);
						else if (strcmp(shaderType, "gs") == 0 && FileExists(path))
							editor->OpenGS(*item);
					}
				}
				else if (type == "pinned") {
					PinnedUI* pinned = ((PinnedUI*)m_ui->Get(ViewID::Pinned));
					if (!settingItem.attribute("name").empty()) {
						const pugi::char_t* item = settingItem.attribute("name").as_string();
						const pugi::char_t* shaderType = settingItem.attribute("from").as_string();
						pipe::ShaderPass* owner = (pipe::ShaderPass*)(m_pipe->Get(settingItem.attribute("owner").as_string())->Data);

						std::vector<ShaderVariable*> vars = owner->Variables.GetVariables();

						for (auto var : vars)
							if (strcmp(var->Name, item) == 0) {
								pinned->Add(var);
								break;
							}
					}
				}
				else if (type == "camera") {
					if (settingItem.attribute("fp").empty())
						Settings::Instance().Project.FPCamera = false;
					else
						Settings::Instance().Project.FPCamera = settingItem.attribute("fp").as_bool();

					SystemVariableManager::Instance().GetCamera()->Reset();

					bool fp = Settings::Instance().Project.FPCamera;

					if (fp) {
						ed::FirstPersonCamera* fpCam = (ed::FirstPersonCamera*)SystemVariableManager::Instance().GetCamera();
						fpCam->Reset();
						fpCam->SetPosition(std::stof(settingItem.child("positionX").text().get()),
							std::stof(settingItem.child("positionY").text().get()),
							std::stof(settingItem.child("positionZ").text().get())
						);
						fpCam->SetYaw(std::stof(settingItem.child("yaw").text().get()));
						fpCam->SetPitch(std::stof(settingItem.child("pitch").text().get()));
					}
					else {
						ed::ArcBallCamera* ab = (ed::ArcBallCamera*)SystemVariableManager::Instance().GetCamera();
						ab->SetDistance(std::stof(settingItem.child("distance").text().get()));
						ab->Yaw(std::stof(settingItem.child("yaw").text().get()));
						ab->Pitch(std::stof(settingItem.child("pitch").text().get()));
						ab->Roll(std::stof(settingItem.child("roll").text().get()));
					}

				}
				else if (type == "clearcolor") {
					if (!settingItem.attribute("r").empty())
						Settings::Instance().Project.ClearColor.r = settingItem.attribute("r").as_float();
					if (!settingItem.attribute("g").empty())
						Settings::Instance().Project.ClearColor.g = settingItem.attribute("g").as_float();
					if (!settingItem.attribute("b").empty())
						Settings::Instance().Project.ClearColor.b = settingItem.attribute("b").as_float();
					if (!settingItem.attribute("a").empty())
						Settings::Instance().Project.ClearColor.a = settingItem.attribute("a").as_float();
				}
			}
		}

		// set actual render texture IDs
		for (auto& pass : fbos) {
			int index = 0;
			for (auto& rtName : pass.second) {
				GLuint rtID = (rtName.size() == 0) ? m_renderer->GetTexture() : m_objects->GetTexture(rtName);
				pass.first->RenderTextures[index] = rtID;
				index++;
			}
		}
	}
}