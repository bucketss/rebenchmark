/*
*
*    This program is free software; you can redistribute it and/or modify it
*    under the terms of the GNU General Public License as published by the
*    Free Software Foundation; either version 2 of the License, or (at
*    your option) any later version.
*
*    This program is distributed in the hope that it will be useful, but
*    WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
*    General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program; if not, write to the Free Software Foundation,
*    Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*
*    In addition, as a special exception, the author gives permission to
*    link the code of this program with the Half-Life Game Engine ("HL
*    Engine") and Modified Game Libraries ("MODs") developed by Valve,
*    L.L.C ("Valve").  You must obey the GNU General Public License in all
*    respects for all of the code used other than the HL Engine and MODs
*    from Valve.  If you modify this file, you may extend this exception
*    to your version of the file, but you are not obligated to do so.  If
*    you do not wish to do so, delete this exception statement from your
*    version.
*
*/

#pragma once

// Where a server frame's time went, marked at the phase boundaries inside
// SV_Frame_Internal. A profiler that holds the engine at arm's length can time
// the game DLL table and the hookchains, but the engine's own phases are
// static functions behind a version script: nothing outside can see where one
// ends and the next begins. These marks say so directly.
//
// The callback is called with begin = 1 on entry and begin = 0 on exit, always
// in pairs and properly nested, so the reader can keep a stack and charge each
// phase its own time. It is called on the server's frame thread only.
//
// Phases are numbered for the life of this interface version: new ones are
// appended, so a reader built against an older header keeps working and simply
// does not know the names of the ones it has never heard of. Ask the engine
// for GetPhaseCount and GetPhaseName rather than assuming this list.

#define VREBENCH_PROBE_VERSION "ReBenchEngineProbe001"

enum RebenchPhase
{
	REBENCH_PHASE_FRAME = 0,        // SV_Frame_Internal, the root of every mark
	REBENCH_PHASE_CHECK_CMD_TIMES,
	REBENCH_PHASE_READ_PACKETS,
	REBENCH_PHASE_PACKET_OOB,       // a connectionless packet: rcon, queries, connects
	REBENCH_PHASE_PACKET_CLIENT,    // one packet from a connected client
	REBENCH_PHASE_PHYSICS,          // SV_Physics, which is where the game DLL runs
	REBENCH_PHASE_MISSING_RESOURCES,
	REBENCH_PHASE_CHECK_TIMEOUTS,
	REBENCH_PHASE_SEND_CLIENT_MESSAGES,
	REBENCH_PHASE_CHECK_MAP_DIFFERENCES,
	REBENCH_PHASE_GATHER_STATISTICS,
	REBENCH_PHASE_STEAM,            // Steam_RunFrame as a whole
	REBENCH_PHASE_STEAM_DETAILS,    // once a second: server details and password state
	REBENCH_PHASE_STEAM_USERDATA,   // once a second: BUpdateUserData for each player
	REBENCH_PHASE_STEAM_CALLBACKS,  // SteamGameServer_RunCallbacks, ten times a second
	REBENCH_PHASE_STEAM_PACKETS,    // draining Steam's outgoing packet queue
	REBENCH_PHASE_NET_RECV,         // recvfrom, inside NET_GetPacket
	REBENCH_PHASE_NET_SEND,         // sendto, inside NET_SendPacket
	REBENCH_PHASE_CONSOLE,          // Con_Printf and Log_Printf, which reach a file or a pipe

	REBENCH_PHASE_COUNT
};

typedef void (*RebenchProbeMarkFn)(int phase, int begin);

class IRebenchProbe {
public:
	virtual ~IRebenchProbe() { }

	// This interface's own version, 1 for the layout above. A reader checks it
	// before calling anything else.
	virtual int GetVersion() = 0;

	virtual int GetPhaseCount() = 0;

	// Stable for the life of the process; the caller may keep the pointer.
	// Null for a phase this engine does not know.
	virtual const char *GetPhaseName(int phase) = 0;

	// Null stops the marking. Only one reader at a time: the caller is
	// expected to check GetMarkCallback first, and to put back what it found
	// when it is done.
	virtual void SetMarkCallback(RebenchProbeMarkFn callback) = 0;

	virtual RebenchProbeMarkFn GetMarkCallback() = 0;
};
