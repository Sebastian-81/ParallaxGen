#include "ParallaxGen/ParallaxGen.hpp"

#include <spdlog/spdlog.h>
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/algorithm/string.hpp>
#include <vector>
#include <fstream>

#include "ParallaxGenUtil/ParallaxGenUtil.hpp"

using namespace std;
namespace fs = filesystem;
using namespace nifly;

ParallaxGen::ParallaxGen(const fs::path output_dir, ParallaxGenDirectory* pgd)
{
    // constructor
    this->output_dir = output_dir;
    this->pgd = pgd;
}

void ParallaxGen::patchMeshes(vector<fs::path>& meshes, vector<fs::path>& heightMaps, vector<fs::path>& complexMaterialMaps)
{
	// patch meshes
	// loop through each mesh nif file
	size_t finished_task = 0;
	size_t num_meshes = meshes.size();
	for (fs::path mesh : meshes) {
		if (finished_task % 100 == 0) {
			double progress = (double)finished_task / num_meshes * 100.0;
			spdlog::info("NIFs Processed: {}/{} ({:.1f}%)", finished_task, num_meshes, progress);
		}

		processNIF(mesh, heightMaps, complexMaterialMaps);
		finished_task++;
	}
}

void ParallaxGen::zipMeshes() {
	// zip meshes
	spdlog::info("Zipping meshes...");
	zipDirectory(output_dir, output_dir / "ParallaxGen_Output.zip");
}

void ParallaxGen::deleteMeshes() {
	// delete meshes
	spdlog::info("Cleaning up meshes generated by ParallaxGen...");
	// Iterate through the folder
	for (const auto& entry : fs::directory_iterator(output_dir)) {
		if (fs::is_directory(entry.path())) {
			// Remove the directory and all its contents
			fs::remove_all(entry.path());
			spdlog::trace("Deleted directory {}", entry.path().string());
		}
	}
}

void ParallaxGen::deleteOutputDir() {
	// delete output directory
	spdlog::info("Deleting existing ParallaxGen output...");
	if (fs::exists(output_dir)) {
		fs::remove_all(output_dir);
	}
}

// shorten some enum names
typedef BSLightingShaderPropertyShaderType BSLSP;
typedef SkyrimShaderPropertyFlags1 SSPF1;
typedef SkyrimShaderPropertyFlags2 SSPF2;
void ParallaxGen::processNIF(fs::path nif_file, vector<fs::path>& heightMaps, vector<fs::path>& complexMaterialMaps)
{
	const fs::path output_file = output_dir / nif_file;
	if (fs::exists(output_file)) {
		spdlog::error("Unable to process NIF file, file already exists: {}", nif_file.string());
		return;
	}

	// process nif file
	vector<std::byte> nif_file_data = pgd->getFile(nif_file);

	boost::iostreams::array_source nif_array_source(reinterpret_cast<const char*>(nif_file_data.data()), nif_file_data.size());
	boost::iostreams::stream<boost::iostreams::array_source> nif_stream(nif_array_source);

	// load nif file
	NifFile nif(nif_stream);
	bool nif_modified = false;

	// ignore nif if has attached havok animations
	//todo: This is not tested
	for (NiNode* node : nif.GetNodes()) {
		string block_name = node->GetBlockName();
		if (block_name == "BSBehaviorGraphExtraData") {
			spdlog::debug("Rejecting NIF file {} due to attached havok animations", nif_file.string());
			return;
		}
	}

	// loop through each node in nif
	size_t shape_id = 0;
	for (NiShape* shape : nif.GetShapes()) {
		// exclusions
		// get shader type
		if (!shape->HasShaderProperty()) {
			spdlog::trace("Rejecting shape {} in NIF file {}: No shader property", shape_id, nif_file.string());
			continue;
		}

		// only allow BSLightingShaderProperty blocks
		string shape_block_name = shape->GetBlockName();
		if (shape_block_name != "NiTriShape" && shape_block_name != "BSTriShape") {
			spdlog::trace("Rejecting shape {} in NIF file {}: Incorrect shape block type", shape_id, nif_file.string());
			continue;
		}

		// ignore skinned meshes, these don't support parallax
		if (shape->HasSkinInstance() || shape->IsSkinned()) {
			spdlog::trace("Rejecting shape {} in NIF file {}: Skinned mesh", shape_id, nif_file.string());
			continue;
		}

		// get shader from shape
		NiShader* shader = nif.GetShader(shape);

		string shader_block_name = shader->GetBlockName();
		if (shader_block_name != "BSLightingShaderProperty") {
			spdlog::trace("Rejecting shape {} in NIF file {}: Incorrect shader block type", shape->GetBlockName(), nif_file.string());
			continue;
		}

		// Ignore if shader type is not 0 (nothing) or 1 (environemnt map) or 3 (parallax)
		BSLSP shader_type = static_cast<BSLSP>(shader->GetShaderType());
		if (shader_type != BSLSP::BSLSP_DEFAULT && shader_type != BSLSP::BSLSP_ENVMAP && shader_type != BSLSP::BSLSP_PARALLAX) {
			spdlog::trace("Rejecting shape {} in NIF file {}: Incorrect shader type", shape->GetBlockName(), nif_file.string());
			continue;
		}

		// build search vector
		vector<string> search_prefixes;
		// diffuse map lookup first
		string diffuse_map;
		uint32_t diffuse_result = nif.GetTextureSlot(shape, diffuse_map, 0);
		if (diffuse_result == 0) {
			continue;
		}
		ParallaxGenUtil::addUniqueElement(search_prefixes, diffuse_map.substr(0, diffuse_map.find_last_of('.')));
		// normal map lookup
		string normal_map;
		uint32_t normal_result = nif.GetTextureSlot(shape, normal_map, 1);
		if (diffuse_result > 0) {
			ParallaxGenUtil::addUniqueElement(search_prefixes, normal_map.substr(0, normal_map.find_last_of('_')));
		}

		// check if complex parallax should be enabled
		for (string& search_prefix : search_prefixes) {
			// check if complex material file exists
			fs::path search_path;
			string search_prefix_lower = boost::algorithm::to_lower_copy(search_prefix);

			// processing for complex material
            search_path = search_prefix_lower + "_m.dds";
            if (find(complexMaterialMaps.begin(), complexMaterialMaps.end(), search_path) != complexMaterialMaps.end()) {
                // Enable complex parallax for this shape!
                nif_modified |= enableComplexMaterialOnShape(nif, shape, shader, search_prefix);
                break;
            }

			// processing for parallax
            search_path = search_prefix_lower + "_p.dds";
            if (find(heightMaps.begin(), heightMaps.end(), search_path) != heightMaps.end()) {
                // Enable regular parallax for this shape!
				if (shader_type != BSLSP::BSLSP_DEFAULT && shader_type != BSLSP::BSLSP_PARALLAX) {
					// this avoids an env map mesh being reverted to parallax mesh
					spdlog::trace("Rejecting shape {} in NIF file {}: Incorrect shader type", shape->GetBlockName(), nif_file.string());
					continue;
				}

                nif_modified |= enableParallaxOnShape(nif, shape, shader, search_prefix);
                break;
            }
		}

		shape_id++;
	}

	// save NIF if it was modified
	if (nif_modified) {
		spdlog::debug("NIF Modified: {}", nif_file.string());

		// create directories if required
		fs::create_directories(output_file.parent_path());

		if (nif.Save(output_file, nif_save_options)) {
			spdlog::error("Unable to save NIF file: {}", nif_file.string());
		}
	}
}

bool ParallaxGen::enableComplexMaterialOnShape(NifFile& nif, NiShape* shape, NiShader* shader, const string& search_prefix)
{
	// enable complex material on shape
	bool changed = false;
	// 1. set shader type to env map
	if (shader->GetShaderType() != BSLSP::BSLSP_ENVMAP) {
		shader->SetShaderType(BSLSP::BSLSP_ENVMAP);
		changed = true;
	}
	// 2. set shader flags
	BSLightingShaderProperty* cur_bslsp = dynamic_cast<BSLightingShaderProperty*>(shader);
	if (!(cur_bslsp->shaderFlags1 & SSPF1::SLSF1_ENVIRONMENT_MAPPING)) {
		cur_bslsp->shaderFlags1 |= SSPF1::SLSF1_ENVIRONMENT_MAPPING;
		changed = true;
	}
	// 3. set vertex colors for shape
	if (!shape->HasVertexColors()) {
		shape->SetVertexColors(true);
		changed = true;
	}
	// 4. set vertex colors for shader
	if (!shader->HasVertexColors()) {
		shader->SetVertexColors(true);
		changed = true;
	}
	// 5. set complex material texture
	string env_map;
	uint32_t env_result = nif.GetTextureSlot(shape, env_map, 5);
	if (env_result == 0 || env_map.empty()) {
		// add height map
		string new_env_map = search_prefix + "_m.dds";
		nif.SetTextureSlot(shape, new_env_map, 5);
		changed = true;
	}
	return changed;
}

bool ParallaxGen::enableParallaxOnShape(NifFile& nif, NiShape* shape, NiShader* shader, const string& search_prefix)
{
	// enable parallax on shape
	bool changed = false;
	// 1. set shader type to parallax
	if (shader->GetShaderType() != BSLSP::BSLSP_PARALLAX) {
		shader->SetShaderType(BSLSP::BSLSP_PARALLAX);
		changed = true;
	}
	// 2. Set shader flags
	BSLightingShaderProperty* cur_bslsp = dynamic_cast<BSLightingShaderProperty*>(shader);
	if (!(cur_bslsp->shaderFlags1 & SSPF1::SLSF1_PARALLAX)) {
		cur_bslsp->shaderFlags1 |= SSPF1::SLSF1_PARALLAX;
		changed = true;
	}
	// 3. set vertex colors for shape
	if (!shape->HasVertexColors()) {
		shape->SetVertexColors(true);
		changed = true;
	}
	// 4. set vertex colors for shader
	if (!shader->HasVertexColors()) {
		shader->SetVertexColors(true);
		changed = true;
	}
	// 5. set parallax heightmap texture
	string height_map;
	uint32_t height_result = nif.GetTextureSlot(shape, height_map, 3);
	if (height_result == 0 || height_map.empty()) {
		// add height map
		string new_height_map = search_prefix + "_p.dds";
		nif.SetTextureSlot(shape, new_height_map, 3);
		changed = true;
	}

	return changed;
}

void ParallaxGen::addFileToZip(mz_zip_archive& zip, const fs::path& filePath, const fs::path& zipPath)
{
	// ignore zip file itself
	if (filePath == zipPath) {
		return;
	}

	// open file stream
	ifstream file(filePath.string(), std::ios::binary);
	vector<char> buffer((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());

	// get relative path
	fs::path zip_relative_path = filePath.lexically_relative(output_dir);
	string zip_file_path = zip_relative_path.string();

	// add file to zip
    if (!mz_zip_writer_add_mem(&zip, zip_file_path.c_str(), buffer.data(), buffer.size(), MZ_NO_COMPRESSION)) {
		spdlog::error("Error adding file to zip: {}", filePath.string());
		ParallaxGenUtil::exitWithUserInput(1);
    }
}

void ParallaxGen::zipDirectory(const fs::path& dirPath, const fs::path& zipPath)
{
	mz_zip_archive zip;

	// init to 0
    memset(&zip, 0, sizeof(zip));

	// initialize file
    if (!mz_zip_writer_init_file(&zip, zipPath.string().c_str(), 0)) {
		spdlog::error("Error creating zip file: {}", zipPath.string());
		ParallaxGenUtil::exitWithUserInput(1);
    }

	// add each file in directory to zip
    for (const auto &entry : fs::recursive_directory_iterator(dirPath)) {
        if (fs::is_regular_file(entry.path())) {
            addFileToZip(zip, entry.path(), zipPath);
        }
    }

	// finalize zip
    if (!mz_zip_writer_finalize_archive(&zip)) {
		spdlog::error("Error finalizing zip archive: {}", zipPath.string());
		ParallaxGenUtil::exitWithUserInput(1);
    }

    mz_zip_writer_end(&zip);
}
