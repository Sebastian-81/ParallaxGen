#ifndef BETHESDAGAME_H
#define BETHESDAGAME_H

#include <unordered_map>
#include <filesystem>
#include <string>

class BethesdaGame {
public:
	enum class GameType {
		SKYRIM_SE,
		SKYRIM_VR,
		SKYRIM
	};

	enum class StoreType {
		STEAM,
		WINDOWS_STORE,
		EPIC_GAMES_STORE,
		GOG
	};

private:
	GameType game_type;
	std::filesystem::path game_path;
	std::filesystem::path game_data_path;

	inline static const std::unordered_map<BethesdaGame::GameType, int> steam_game_ids = {
		{BethesdaGame::GameType::SKYRIM_SE, 489830},
		{BethesdaGame::GameType::SKYRIM_VR, 611670},
		{BethesdaGame::GameType::SKYRIM, 72850}
	};

public:
	BethesdaGame(GameType game_type, std::string game_path = "");

	GameType getGameType() const;
	std::filesystem::path getGamePath() const;
	std::filesystem::path getGameDataPath() const;
	std::filesystem::path findGamePathFromSteam() const;

private:
	int getSteamGameID() const;
};

#endif
