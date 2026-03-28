/**
 * GamePacketHandler — Packet routing with lobby/room support.
 *
 * Handles both lobby operations (room list, create, join, leave)
 * and in-game operations (input relay, config, ready).
 *
 * Lobby operations are game-agnostic. Only the GAME_CONFIG payload
 * format is game-specific (and the server just relays it opaquely).
 *
 * For a new decomp game:
 *   1. Copy this file (or reuse as-is — it's game-agnostic!)
 *   2. Only change packet IDs if your client uses different ones
 */

import java.io.*;
import java.nio.*;
import java.util.*;
import NetLib.*;

public class GamePacketHandler {

    // ---- Packet IDs (must match N64 client netplay_core.h) ----

    // Client → Server: Lobby
    public static final int PKTID_CONNECT        = 0x00;
    public static final int PKTID_LIST_ROOMS     = 0x05;
    public static final int PKTID_CREATE_ROOM    = 0x06;
    public static final int PKTID_JOIN_ROOM      = 0x07;
    public static final int PKTID_LEAVE_ROOM     = 0x08;

    // Client → Server: In-Game
    public static final int PKTID_PLAYER_INPUT   = 0x01;
    public static final int PKTID_READY          = 0x02;
    public static final int PKTID_GAME_CONFIG    = 0x03;
    public static final int PKTID_HEARTBEAT_ACK  = 0x04;

    // Server → Client: Lobby
    public static final int PKTID_ASSIGN_PLAYER  = 0x10;
    public static final int PKTID_ROOM_LIST      = 0x16;
    public static final int PKTID_ROOM_JOINED    = 0x17;
    public static final int PKTID_ROOM_UPDATE    = 0x18;
    public static final int PKTID_ROOM_ERROR     = 0x19;

    // Server → Client: In-Game
    public static final int PKTID_REMOTE_INPUT   = 0x11;
    public static final int PKTID_ALL_READY      = 0x12;
    public static final int PKTID_SERVER_CONFIG  = 0x13;
    public static final int PKTID_PLAYER_LEFT    = 0x14;
    public static final int PKTID_HEARTBEAT      = 0x15;

    // Error codes
    public static final int ERR_ROOM_FULL       = 1;
    public static final int ERR_ROOM_NOT_FOUND  = 2;
    public static final int ERR_WRONG_PASSWORD  = 3;
    public static final int ERR_ROOM_IN_GAME    = 4;

    // Room storage
    private static final List<Room> rooms = Collections.synchronizedList(new ArrayList<>());

    /**
     * Handle initial connection (not yet in a room).
     */
    public static void handleConnect(ClientConnection client) {
        // Send player assignment (temporary — real slot assigned on room join)
        byte[] data = new byte[] { 0, 0 };
        client.queueOutgoing(new NetLibPacket(PKTID_ASSIGN_PLAYER, data));
        System.out.println("Client connected from " + client.getName());
    }

    /**
     * Route a packet from a connected client.
     */
    public static void handlePacket(ClientConnection client, NetLibPacket pkt) {
        switch (pkt.GetType()) {
            // --- Lobby ---
            case PKTID_LIST_ROOMS:
                handleListRooms(client);
                break;
            case PKTID_CREATE_ROOM:
                handleCreateRoom(client, pkt);
                break;
            case PKTID_JOIN_ROOM:
                handleJoinRoom(client, pkt);
                break;
            case PKTID_LEAVE_ROOM:
                handleLeaveRoom(client);
                break;

            // --- In-Game ---
            case PKTID_PLAYER_INPUT:
                handlePlayerInput(client, pkt);
                break;
            case PKTID_READY:
                handleReady(client);
                break;
            case PKTID_GAME_CONFIG:
                handleGameConfig(client, pkt);
                break;
            case PKTID_HEARTBEAT_ACK:
                break;
            default:
                break;
        }
    }

    /**
     * Handle client disconnect — remove from room, notify others.
     */
    public static void handleDisconnect(ClientConnection client) {
        Room room = client.getRoom();
        int slot = client.getRoomSlot();
        if (room != null && slot >= 0) {
            room.removePlayer(slot);
            // Notify remaining players
            byte[] updateData = new byte[] {
                (byte) room.getPlayerCount(),
                (byte) slot,
                (byte) 0  // 0 = left
            };
            room.broadcastAll(new NetLibPacket(PKTID_ROOM_UPDATE, updateData));
            room.broadcastAll(new NetLibPacket(PKTID_PLAYER_LEFT, new byte[] { (byte) slot }));

            // Clean up empty rooms
            if (room.isEmpty()) {
                rooms.remove(room);
                System.out.println("Room '" + room.name + "' removed (empty)");
            }
        }
    }

    // ---- Lobby Operations ----

    private static void handleListRooms(ClientConnection client) {
        try {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            synchronized (rooms) {
                int count = Math.min(rooms.size(), 16);
                out.write((byte) count);
                for (int i = 0; i < count; i++) {
                    out.write(rooms.get(i).toListEntry());
                }
            }
            client.queueOutgoing(new NetLibPacket(PKTID_ROOM_LIST, out.toByteArray()));
        } catch (Exception e) {
            System.err.println("Error building room list: " + e.getMessage());
        }
    }

    private static void handleCreateRoom(ClientConnection client, NetLibPacket pkt) {
        byte[] data = pkt.GetData();
        if (data == null || data.length < 3) return;

        ByteBuffer buf = ByteBuffer.wrap(data);
        int nameLen = buf.get() & 0xFF;
        byte[] nameBytes = new byte[Math.min(nameLen, 19)];
        buf.get(nameBytes);
        String name = new String(nameBytes);
        int maxPlayers = buf.get() & 0xFF;
        if (maxPlayers < 2) maxPlayers = 2;
        if (maxPlayers > 8) maxPlayers = 8;

        Room room = new Room(name, maxPlayers);
        int slot = room.addPlayer(client);
        client.setRoom(room, slot);
        rooms.add(room);

        // Send room joined confirmation: [room_id:2][slot:1][count:1][host:1]
        byte[] response = ByteBuffer.allocate(5)
            .putShort((short) room.id)
            .put((byte) slot)
            .put((byte) room.getPlayerCount())
            .put((byte) 1)  // is host
            .array();
        client.queueOutgoing(new NetLibPacket(PKTID_ROOM_JOINED, response));

        System.out.println("Room '" + name + "' created (max " + maxPlayers + ") by client");
    }

    private static void handleJoinRoom(ClientConnection client, NetLibPacket pkt) {
        byte[] data = pkt.GetData();
        if (data == null || data.length < 3) return;

        ByteBuffer buf = ByteBuffer.wrap(data);
        int roomId = buf.getShort() & 0xFFFF;

        Room room = null;
        synchronized (rooms) {
            for (Room r : rooms) {
                if (r.id == roomId) { room = r; break; }
            }
        }

        if (room == null) {
            client.queueOutgoing(new NetLibPacket(PKTID_ROOM_ERROR, new byte[] { ERR_ROOM_NOT_FOUND }));
            return;
        }
        if (room.inGame) {
            client.queueOutgoing(new NetLibPacket(PKTID_ROOM_ERROR, new byte[] { ERR_ROOM_IN_GAME }));
            return;
        }
        if (room.isFull()) {
            client.queueOutgoing(new NetLibPacket(PKTID_ROOM_ERROR, new byte[] { ERR_ROOM_FULL }));
            return;
        }

        int slot = room.addPlayer(client);
        if (slot < 0) {
            client.queueOutgoing(new NetLibPacket(PKTID_ROOM_ERROR, new byte[] { ERR_ROOM_FULL }));
            return;
        }
        client.setRoom(room, slot);

        // Send joined confirmation
        byte[] response = ByteBuffer.allocate(5)
            .putShort((short) room.id)
            .put((byte) slot)
            .put((byte) room.getPlayerCount())
            .put((byte) 0)  // not host
            .array();
        client.queueOutgoing(new NetLibPacket(PKTID_ROOM_JOINED, response));

        // Notify others in room
        byte[] updateData = new byte[] {
            (byte) room.getPlayerCount(),
            (byte) slot,
            (byte) 1  // 1 = joined
        };
        room.broadcastExcept(slot, new NetLibPacket(PKTID_ROOM_UPDATE, updateData));

        // Send existing game config to new player if host already set it
        if (room.gameConfig != null) {
            client.queueOutgoing(new NetLibPacket(PKTID_SERVER_CONFIG, room.gameConfig));
        }

        System.out.println("Player joined room '" + room.name + "' as slot " + slot);
    }

    private static void handleLeaveRoom(ClientConnection client) {
        Room room = client.getRoom();
        int slot = client.getRoomSlot();
        if (room == null) return;

        room.removePlayer(slot);
        client.setRoom(null, -1);

        byte[] updateData = new byte[] {
            (byte) room.getPlayerCount(),
            (byte) slot,
            (byte) 0  // 0 = left
        };
        room.broadcastAll(new NetLibPacket(PKTID_ROOM_UPDATE, updateData));

        if (room.isEmpty()) {
            rooms.remove(room);
        }
    }

    // ---- In-Game Operations ----

    private static void handlePlayerInput(ClientConnection client, NetLibPacket pkt) {
        Room room = client.getRoom();
        int slot = client.getRoomSlot();
        if (room == null || slot < 0) return;

        byte[] clientData = pkt.GetData();
        if (clientData == null || clientData.length < 9) return;

        // Data already has [slot:1][frame:4][input:4] — relay as REMOTE_INPUT
        NetLibPacket relay = new NetLibPacket(PKTID_REMOTE_INPUT, clientData, PacketFlag.FLAG_UNRELIABLE);
        room.broadcastExcept(slot, relay);
    }

    private static void handleReady(ClientConnection client) {
        Room room = client.getRoom();
        int slot = client.getRoomSlot();
        if (room == null || slot < 0) return;

        System.out.println("Player " + slot + " ready in room '" + room.name + "'");

        if (room.setReady(slot)) {
            System.out.println("All players ready in room '" + room.name + "' — starting!");
            room.inGame = true;
            room.broadcastAll(new NetLibPacket(PKTID_ALL_READY, null));
        }
    }

    private static void handleGameConfig(ClientConnection client, NetLibPacket pkt) {
        Room room = client.getRoom();
        int slot = client.getRoomSlot();
        if (room == null || slot != 0) return; // Only host (slot 0)

        byte[] config = pkt.GetData();
        if (config == null) return;

        room.gameConfig = config;
        room.broadcastAll(new NetLibPacket(PKTID_SERVER_CONFIG, config));
        System.out.println("Game config set by host in room '" + room.name + "'");
    }
}
