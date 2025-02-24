#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "game/game.hpp"
#include "game/dvars.hpp"

#include "command.hpp"
#include "console.hpp"
#include "dvars.hpp"
#include "network.hpp"
#include "scheduler.hpp"
#include "server_list.hpp"
#include "party.hpp"

#include "steam/steam.hpp"

#include <utils/string.hpp>
#include <utils/info_string.hpp>
#include <utils/cryptography.hpp>
#include <utils/hook.hpp>

#include <version.hpp>

namespace party
{
	namespace
	{
		struct
		{
			game::netadr_s host{};
			std::string challenge{};
			bool hostDefined{false};
		} connect_state;

		utils::hook::detour didyouknow_hook;

		std::string sv_motd;

		int sv_maxclients;

		void switch_gamemode_if_necessary(const std::string& gametype)
		{
			const auto target_mode = gametype == "aliens" ? game::CODPLAYMODE_ALIENS : game::CODPLAYMODE_CORE;
			const auto current_mode = game::Com_GetCurrentCoDPlayMode();

			if (current_mode != target_mode)
			{
				switch (target_mode)
				{
				case game::CODPLAYMODE_CORE:
					game::SwitchToCoreMode();
					break;
				case game::CODPLAYMODE_ALIENS:
					game::SwitchToAliensMode();
					break;
				}
			}
		}

		void perform_game_initialization()
		{
			// This fixes several crashes and impure client stuff
			command::execute("onlinegame 1", true);
			command::execute("exec default_xboxlive.cfg", true);
			command::execute("xstartprivateparty", true);
			command::execute("xblive_privatematch 0", true);
			command::execute("startentitlements", true);
		}

		void start_map(const std::string& mapname)
		{
			if (game::Live_SyncOnlineDataFlags(0) != 0)
			{
				scheduler::on_game_initialized([mapname]
				{
					command::execute("map " + mapname, false);
				}, scheduler::pipeline::main, 1s);
			}
			else
			{
				switch_gamemode_if_necessary(dvars::get_string("g_gametype"));

				if (!game::environment::is_dedi())
				{
					perform_game_initialization();
				}

				auto* current_mapname = game::Dvar_FindVar("mapname");
				if (current_mapname && utils::string::to_lower(current_mapname->current.string) == utils::string::to_lower(mapname) && game::SV_Loaded())
				{
					console::info("Restarting map: %s\n", mapname.data());
					command::execute("map_restart", false);
					return;
				}

				console::info("Starting map: %s\n", mapname.data());
				game::SV_StartMapForParty(0, mapname.data(), false, false);
			}
		}

		void connect_to_party(const game::netadr_s& target, const std::string& mapname, const std::string& gametype)
		{
			if (game::environment::is_sp())
			{
				return;
			}

			if (game::Live_SyncOnlineDataFlags(0))
			{
				scheduler::once([=]()
				{
					connect_to_party(target, mapname, gametype);
				}, scheduler::pipeline::main, 1s);
				return;
			}

			switch_gamemode_if_necessary(gametype);
			perform_game_initialization();

			// CL_ConnectFromParty
			char session_info[0x100] = {};
			reinterpret_cast<void(*)(int, char*, const game::netadr_s*, const char*, const char*)>(0x1402C5700)(
				0, session_info, &target, mapname.data(), gametype.data());
		}

		void disconnect_stub()
		{
			if (game::CL_IsCgameInitialized())
			{
				// CL_ForwardCommandToServer
				reinterpret_cast<void (*)(int, const char*)>(0x1402C76C0)(0, "disconnect");
				// CL_WritePacket
				reinterpret_cast<void (*)(int)>(0x1402C1E70)(0);
			}
			// CL_Disconnect
			reinterpret_cast<void (*)(int)>(0x1402C6240)(0);
		}

		const auto drop_reason_stub = utils::hook::assemble([](utils::hook::assembler& a)
		{
			a.mov(rdx, rdi);
			a.mov(ecx, 2);
			a.jmp(0x1402C617D);
		});
	}

	int server_client_count()
	{
		return sv_maxclients;
	}

	int get_client_count()
	{
		auto count = 0;
		for (auto i = 0; i < *game::mp::svs_numclients; ++i)
		{
			if (game::mp::svs_clients[i].header.state >= game::CS_CONNECTED)
			{
				++count;
			}
		}

		return count;
	}

	int get_bot_count()
	{
		auto count = 0;
		for (auto i = 0; i < *game::mp::svs_numclients; ++i)
		{
			if (game::mp::svs_clients[i].header.state >= game::CS_CONNECTED &&
				game::mp::svs_clients[i].testClient != game::TC_NONE)
			{
				++count;
			}
		}

		return count;
	}

	void reset_connect_state()
	{
		connect_state = {};
	}

	int get_client_num_from_name(const std::string& name)
	{
		for (auto i = 0; !name.empty() && i < *game::mp::svs_numclients; ++i)
		{
			if (game::mp::g_entities[i].client)
			{
				char client_name[16] = {0};
				strncpy_s(client_name, game::mp::g_entities[i].client->sess.cs.name, sizeof(client_name));
				game::I_CleanStr(client_name);

				if (client_name == name)
				{
					return i;
				}
			}
		}
		return -1;
	}

	void connect(const game::netadr_s& target)
	{
		if (game::environment::is_sp())
		{
			return;
		}

		command::execute("lui_open popup_acceptinginvite", false);

		connect_state.host = target;
		connect_state.challenge = utils::cryptography::random::get_challenge();
		connect_state.hostDefined = true;

		network::send(target, "getInfo", connect_state.challenge);
	}

	void map_restart()
	{
		if (!game::SV_Loaded())
		{
			return;
		}
		*reinterpret_cast<int*>(0x144DB8C84) = 1; // sv_map_restart
		*reinterpret_cast<int*>(0x144DB8C88) = 1; // sv_loadScripts
		*reinterpret_cast<int*>(0x144DB8C8C) = 0; // sv_migrate
		reinterpret_cast<void(*)()>(0x14046F3B0)(); // SV_CheckLoadGame
	}

	void didyouknow_stub(game::dvar_t* dvar, const char* string)
	{
		if (dvar->name == "didyouknow"s && !party::sv_motd.empty())
		{
			string = party::sv_motd.data();
		}

		return didyouknow_hook.invoke<void>(dvar, string);
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			if (game::environment::is_sp())
			{
				return;
			}

			// hook disconnect command function
			utils::hook::jump(0x1402C6370, disconnect_stub);

			if (game::environment::is_mp())
			{
				// show custom drop reason
				utils::hook::nop(0x1402C6100, 13);
				utils::hook::jump(0x1402C6100, drop_reason_stub, true);
			}

			// enable custom kick reason in GScr_KickPlayer
			utils::hook::set<uint8_t>(0x1403CBFD0, 0xEB);

			didyouknow_hook.create(game::Dvar_SetString, didyouknow_stub);

			command::add("map", [](const command::params& argument)
			{
				if (argument.size() != 2)
				{
					return;
				}

				start_map(argument[1]);
			});

			command::add("map_restart", map_restart);

			command::add("fast_restart", []
			{
				if (game::SV_Loaded())
				{
					game::SV_FastRestart();
				}
			});

			command::add("reconnect", [](const command::params& argument)
			{
				if (!connect_state.hostDefined)
				{
					console::info("Cannot connect to server.\n");
					return;
				}
				
				if (game::CL_IsCgameInitialized())
				{
					command::execute("disconnect");
					command::execute("reconnect");
				}
				else
				{
					connect(connect_state.host);
				}
			});

			command::add("connect", [](const command::params& argument)
			{
				if (argument.size() != 2)
				{
					return;
				}

				game::netadr_s target{};
				if (game::NET_StringToAdr(argument[1], &target))
				{
					connect(target);
				}
			});

			command::add("clientkick", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					console::info("usage: clientkick <num>, <reason>(optional)\n");
					return;
				}

				if (!game::SV_Loaded())
				{
					return;
				}

				std::string reason;
				if (params.size() > 2)
				{
					reason = params.join(2);
				}
				if (reason.empty())
				{
					reason = "EXE_PLAYERKICKED";
				}

				const auto client_num = atoi(params.get(1));
				if (client_num < 0 || client_num >= *game::mp::svs_numclients)
				{
					return;
				}

				scheduler::once([client_num, reason]
				{
					game::SV_KickClientNum(client_num, reason.data());
				}, scheduler::pipeline::server);
			});

			command::add("kick", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					console::info("usage: kick <name>, <reason>(optional)\n");
					return;
				}

				if (!game::SV_Loaded())
				{
					return;
				}

				std::string reason;
				if (params.size() > 2)
				{
					reason = params.join(2);
				}
				if (reason.empty())
				{
					reason = "EXE_PLAYERKICKED";
				}

				const std::string name = params.get(1);
				if (name == "all"s)
				{
					for (auto i = 0; i < *game::mp::svs_numclients; ++i)
					{
						scheduler::once([i, reason]
						{
							game::SV_KickClientNum(i, reason.data());
						}, scheduler::pipeline::server);
					}
					return;
				}

				const auto client_num = get_client_num_from_name(name);
				if (client_num < 0 || client_num >= *game::mp::svs_numclients)
				{
					return;
				}

				scheduler::once([client_num, reason]
				{
					game::SV_KickClientNum(client_num, reason.data());
				}, scheduler::pipeline::server);
			});

			scheduler::once([]
			{
				game::Dvar_RegisterString("sv_sayName", "console", game::DvarFlags::DVAR_FLAG_NONE,
				                          "The name to pose as for 'say' commands");
				game::Dvar_RegisterString("didyouknow", "", game::DvarFlags::DVAR_FLAG_NONE, "");
			}, scheduler::pipeline::main);

			command::add("tell", [](const command::params& params)
			{
				if (params.size() < 3)
				{
					return;
				}

				const auto client_num = atoi(params.get(1));
				const auto message = params.join(2);
				const auto* const name = game::Dvar_FindVar("sv_sayName")->current.string;

				game::SV_GameSendServerCommand(client_num, 0, utils::string::va("%c \"%s: %s\"", 84, name, message.data()));
				console::info("%s -> %i: %s\n", name, client_num, message.data());
			});

			command::add("tellraw", [](const command::params& params)
			{
				if (params.size() < 3)
				{
					return;
				}

				const auto client_num = atoi(params.get(1));
				const auto message = params.join(2);

				game::SV_GameSendServerCommand(client_num, 0, utils::string::va("%c \"%s\"", 84, message.data()));
				console::info("%i: %s\n", client_num, message.data());
			});

			command::add("say", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					return;
				}

				const auto message = params.join(1);
				const auto* const name = game::Dvar_FindVar("sv_sayName")->current.string;

				game::SV_GameSendServerCommand(-1, 0, utils::string::va("%c \"%s: %s\"", 84, name, message.data()));
				console::info("%s: %s\n", name, message.data());
			});

			command::add("sayraw", [](const command::params& params)
			{
				if (params.size() < 2)
				{
					return;
				}

				const auto message = params.join(1);

				game::SV_GameSendServerCommand(-1, 0, utils::string::va("%c \"%s\"", 84, message.data()));
				console::info("%s\n", message.data());
			});

			network::on("getInfo", [](const game::netadr_s& target, const std::string& data)
			{
				utils::info_string info;
				info.set("challenge", data);
				info.set("gamename", "IW6");
				info.set("hostname", dvars::get_string("sv_hostname"));
				info.set("gametype", dvars::get_string("g_gametype"));
				info.set("sv_motd", dvars::get_string("sv_motd"));
				info.set("xuid", utils::string::va("%llX", steam::SteamUser()->GetSteamID().bits));
				info.set("mapname", dvars::get_string("mapname"));
				info.set("isPrivate", dvars::get_string("g_password").empty() ? "0" : "1");
				info.set("clients", std::to_string(get_client_count()));
				info.set("bots", std::to_string(get_bot_count()));
				info.set("sv_maxclients", std::to_string(*game::mp::svs_numclients));
				info.set("protocol", std::to_string(PROTOCOL));
				info.set("shortversion", SHORTVERSION);

				network::send(target, "infoResponse", info.build(), '\n');
			});

			if (game::environment::is_dedi())
			{
				return;
			}

			network::on("infoResponse", [](const game::netadr_s& target, const std::string& data)
			{
				const utils::info_string info(data);
				server_list::handle_info_response(target, info);

				if (connect_state.host != target)
				{
					return;
				}

				if (info.get("challenge") != connect_state.challenge)
				{
					console::info("Invalid challenge.\n");
					return;
				}

				const auto mapname = info.get("mapname");
				if (mapname.empty())
				{
					console::info("Invalid map.\n");
					return;
				}

				const auto gametype = info.get("gametype");
				if (gametype.empty())
				{
					console::info("Invalid gametype.\n");
					return;
				}

				const auto gamename = info.get("gamename");
				if (gamename != "IW6"s)
				{
					console::info("Invalid gamename.\n");
					return;
				}

				sv_motd = info.get("sv_motd");

				try
				{
					sv_maxclients = std::stoi(info.get("sv_maxclients"));
				}
				catch ([[maybe_unused]] const std::exception& ex)
				{
					sv_maxclients = 1;
				}

				connect_to_party(target, mapname, gametype);
			});
		}
	};
}

REGISTER_COMPONENT(party::component)
