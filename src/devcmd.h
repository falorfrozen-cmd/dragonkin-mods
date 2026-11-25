#pragma once
#include <string>
// Developer command channel used for automated testing from outside the game.
// Write commands (one per line) to DragonkinMod_cmd.txt next to the DLL; they run on
// the game thread and the results are appended to DragonkinMod_out.txt.
//
//   find <Class> [max]                 live instances of a class
//   props <target> [filter]            properties with values
//   get <target> <prop> / set <target> <prop> <value>
//   sig <target|Class> <Function>      parameters of a function
//   call <target> <Function> [p=v ...] call a function (ProcessEvent), prints out/return values
//   callall <Class> <Function> [p=v]   call on every live instance
//   attrs [filter] / setattr <name> <value>
//   mod <setting> <value> / modstate
//   pos / tp <x> <y> <z> / exec <console command> / wait <ms> / echo <text>
//   qol                                QoL state and DPS meter
//   action <unstuck|savepos n|loadpos n|selljunk|resetdps|potion>
//   enemyattrs <asc> / modattr <asc> <Attr> <value> [op 0=add 1=mul 2=div 3=override]
//
// Targets: pawn pc ps world engine gi attrset | cdo:<Class> | first:<Class> | idx:<n> | near:<Class>
namespace dev {

void PollCommandFile();  // worker thread
void RunPending();       // game thread
// Runs one command line right now (game thread) and returns its output. Used by the
// mod loader for "command" actions and by the in-panel developer console.
std::string Exec(const std::string& line);
// Resolves a target the same way the commands do (pawn, pc, cdo:Class, first:Class, idx:N, a.b.c paths).
void* ResolveTarget(const std::string& target);

}  // namespace dev
