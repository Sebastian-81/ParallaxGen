#include "ParallaxGenDirectory/ParallaxGenDirectory.hpp"

#include <spdlog/spdlog.h>
#include <DirectXTex.h>
#include <boost/algorithm/string.hpp>
#include <set>

#include "ParallaxGenUtil/ParallaxGenUtil.hpp"

using namespace std;
using namespace ParallaxGenUtil;

ParallaxGenDirectory::ParallaxGenDirectory(BethesdaGame bg, filesystem::path EXE_PATH) : BethesdaDirectory(bg, true) {
	this->EXE_PATH = EXE_PATH;
}

void ParallaxGenDirectory::findHeightMaps()
{
	// find height maps
	spdlog::info("Finding parallax height maps");

	// Get relevant lists from config
	vector<wstring> parallax_allowlist = jsonArrayToWString(PG_config["parallax_lookup"]["allowlist"]);
	vector<wstring> parallax_blocklist = jsonArrayToWString(PG_config["parallax_lookup"]["blocklist"]);
	vector<wstring> parallax_archive_blocklist = jsonArrayToWString(PG_config["parallax_lookup"]["archive_blocklist"]);

	// Find heightmaps
	heightMaps = findFilesBySuffix("_p.dds", true, parallax_allowlist, parallax_blocklist, parallax_archive_blocklist);
	spdlog::info("Found {} height maps", heightMaps.size());
}

void ParallaxGenDirectory::findComplexMaterialMaps()
{
	// TODO GPU acceleration for this part

	spdlog::info("Finding complex material maps");

	// Get relevant lists from config
	vector<wstring> complex_material_allowlist = jsonArrayToWString(PG_config["complexmaterial_lookup"]["allowlist"]);
	vector<wstring> complex_material_blocklist = jsonArrayToWString(PG_config["complexmaterial_lookup"]["blocklist"]);
	vector<wstring> complex_material_archive_blocklist = jsonArrayToWString(PG_config["complexmaterial_lookup"]["archive_blocklist"]);

	// find complex material maps
	vector<filesystem::path> env_maps = findFilesBySuffix("_m.dds", true, complex_material_allowlist, complex_material_blocklist, complex_material_archive_blocklist);

	// loop through env maps
	for (filesystem::path env_map : env_maps) {
		// check if env map is actually a complex material map
		const vector<std::byte> env_map_data = getFile(env_map);

		// load image into directxtex
		DirectX::ScratchImage image;
		HRESULT hr = DirectX::LoadFromDDSMemory(env_map_data.data(), env_map_data.size(), DirectX::DDS_FLAGS_NONE, nullptr, image);
		if (FAILED(hr)) {
			spdlog::warn(L"Failed to load DDS from memory: {} - skipping", env_map.wstring());
			continue;
		}

		// check if image is a complex material map
		if (!image.IsAlphaAllOpaque()) {
			// if alpha channel is used, there is parallax data
			// this won't work on complex matterial maps that don't make use of complex parallax
			// I'm not sure there's a way to check for those other cases
			spdlog::trace(L"Adding {} as a complex material map", env_map.wstring());
			complexMaterialMaps.push_back(env_map);
		}
	}

	spdlog::info("Found {} complex material maps", complexMaterialMaps.size());
}

void ParallaxGenDirectory::findMeshes()
{
	// find meshes
	spdlog::info("Finding meshes");

	// get relevant lists
	vector<wstring> mesh_allowlist = jsonArrayToWString(PG_config["nif_lookup"]["allowlist"]);
	vector<wstring> mesh_blocklist = jsonArrayToWString(PG_config["nif_lookup"]["blocklist"]);
	vector<wstring> mesh_archive_blocklist = jsonArrayToWString(PG_config["nif_lookup"]["archive_blocklist"]);

	meshes = findFilesBySuffix(".nif", true, mesh_allowlist, mesh_blocklist, mesh_archive_blocklist);
	spdlog::info("Found {} meshes", meshes.size());
}

void ParallaxGenDirectory::findTruePBRConfigs()
{
	// Find True PBR configs
	spdlog::info("Finding TruePBR configs");
  
  	// get relevant lists
	vector<wstring> truepbr_allowlist = jsonArrayToWString(PG_config["truepbr_cfg_lookup"]["allowlist"]);
	vector<wstring> truepbr_blocklist = jsonArrayToWString(PG_config["truepbr_cfg_lookup"]["blocklist"]);
	vector<wstring> truepbr_archive_blocklist = jsonArrayToWString(PG_config["truepbr_cfg_lookup"]["archive_blocklist"]);
  
	auto config_files = findFilesBySuffix(".json", true, truepbr_allowlist, truepbr_blocklist, truepbr_archive_blocklist);

	// loop through and parse configs
	for (auto& config : config_files) {
		// check if config is valid
		auto config_file_bytes = getFile(config);
		string config_file_str(reinterpret_cast<const char*>(config_file_bytes.data()), config_file_bytes.size());

		try {
			nlohmann::json j = nlohmann::json::parse(config_file_str);
			// loop through each element
			for (auto& element : j) {
				// Preprocessing steps here
				if (element.contains("texture")) {
					element["match_diffuse"] = element["texture"];
				}

				// loop through filename fields
				for (const auto& field : truePBR_filename_fields) {
					if (element.contains(field)) {
						element[field] = element[field].get<string>().insert(0, 1, '\\');
					}
				}

				truePBRConfigs.push_back(element);
			}
		}
		catch (nlohmann::json::parse_error& e) {
			spdlog::error(L"Unable to parse TruePBR config file {}: {}", config.wstring(), convertToWstring(e.what()));
      		continue;
		}
	}
  
	spdlog::info("Found {} TruePBR entries", truePBRConfigs.size());
}

void ParallaxGenDirectory::loadPGConfig(bool load_default)
{
	spdlog::info("Loading ParallaxGen configs from load order");

	// Load default config
	if (load_default) {
		filesystem::path def_conf_path = EXE_PATH / "cfg/default.json";
		if (!filesystem::exists(def_conf_path)) {
			spdlog::error(L"Default config not found at {}", def_conf_path.wstring());
			exitWithUserInput(1);
		}

		PG_config.merge_patch(nlohmann::json::parse(getFileBytes(def_conf_path)));
	}

	// Load configs from load order
	size_t cfg_count = 0;
	auto pg_configs = findFilesBySuffix(".json", true, { LO_PGCONFIG_PATH + L"\\*" });
	for (auto& cur_cfg : pg_configs) {
		try {
			auto parsed_json = nlohmann::json::parse(getFile(cur_cfg));
			merge_json_smart(PG_config, parsed_json);
			cfg_count++;
		} catch (nlohmann::json::parse_error& e) {
			spdlog::warn(L"Failed to parse ParallaxGen config file {}: {}", cur_cfg.wstring(), convertToWstring(e.what()));
			continue;
		}
	}

  	// Loop through each element in JSON
	replaceForwardSlashes(PG_config);

	spdlog::info("Loaded {} ParallaxGen configs from load order", cfg_count);
}

void ParallaxGenDirectory::addHeightMap(filesystem::path path)
{
	filesystem::path path_lower = getPathLower(path);

	// add to vector
	addUniqueElement(heightMaps, path_lower);
}

void ParallaxGenDirectory::addComplexMaterialMap(filesystem::path path)
{
	filesystem::path path_lower = getPathLower(path);

	// add to vector
	addUniqueElement(complexMaterialMaps, path_lower);
}

void ParallaxGenDirectory::addMesh(filesystem::path path)
{
	filesystem::path path_lower = getPathLower(path);

	// add to vector
	addUniqueElement(meshes, path_lower);
}

bool ParallaxGenDirectory::isHeightMap(filesystem::path path) const
{
	return find(heightMaps.begin(), heightMaps.end(), getPathLower(path)) != heightMaps.end();
}

bool ParallaxGenDirectory::isComplexMaterialMap(filesystem::path path) const
{
	return find(complexMaterialMaps.begin(), complexMaterialMaps.end(), getPathLower(path)) != complexMaterialMaps.end();
}

bool ParallaxGenDirectory::isMesh(filesystem::path path) const
{
	return find(meshes.begin(), meshes.end(), getPathLower(path)) != meshes.end();
}

bool ParallaxGenDirectory::defCubemapExists()
{
	return isFile(default_cubemap_path);
}

const vector<filesystem::path> ParallaxGenDirectory::getHeightMaps() const
{
	return heightMaps;
}

const vector<filesystem::path> ParallaxGenDirectory::getComplexMaterialMaps() const
{
	return complexMaterialMaps;
}

const vector<filesystem::path> ParallaxGenDirectory::getMeshes() const
{
	return meshes;
}

const vector<nlohmann::json> ParallaxGenDirectory::getTruePBRConfigs() const
{
	return truePBRConfigs;
}

void ParallaxGenDirectory::merge_json_smart(nlohmann::json& target, const nlohmann::json& source) {
    // recursively merge json objects while preseving lists
	for (auto& [key, value] : source.items()) {
		if (value.is_object()) {
			if (!target.contains(key)) {
				target[key] = nlohmann::json::object();
			}
			merge_json_smart(target[key], value);
		} else if (value.is_array()) {
			if (!target.contains(key)) {
				target[key] = nlohmann::json::array();
			}
			for (const auto& item : value) {
				if (std::find(target[key].begin(), target[key].end(), item) == target[key].end()) {
					target[key].push_back(item);
				}
			}
		} else {
			target[key] = value;
		}
	}
}

vector<wstring> ParallaxGenDirectory::jsonArrayToWString(const nlohmann::json& json_array) {
    vector<wstring> result;

    if (json_array.is_array()) {
        for (const auto& item : json_array) {
            if (item.is_string()) {
                result.push_back(convertToWstring(item.get<string>()));
            }
        }
    }

    return result;
}

void ParallaxGenDirectory::replaceForwardSlashes(nlohmann::json& j) {
    if (j.is_string()) {
        std::string& str = j.get_ref<std::string&>();
        for (auto& ch : str) {
            if (ch == '/') {
                ch = '\\';
            }
        }
    } else if (j.is_object()) {
        for (auto& item : j.items()) {
            replaceForwardSlashes(item.value());
        }
    } else if (j.is_array()) {
        for (auto& element : j) {
            replaceForwardSlashes(element);
        }
    }
}
