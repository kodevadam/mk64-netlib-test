/**
 * Room — A game room within the server.
 *
 * Players create and join rooms. Once all are ready, the game starts.
 * Game-agnostic — no game-specific logic here.
 */

import java.nio.*;
import java.util.*;
import java.util.concurrent.*;
import NetLib.*;

public class Room {

    private static int nextRoomId = 1;

    public final int id;
    public final String name;
    public final int maxPlayers;
    public volatile boolean inGame = false;
    public volatile byte[] gameConfig = null;

    private final ClientConnection[] players;
    private final boolean[] ready;
    private volatile int connectedCount = 0;

    public Room(String name, int maxPlayers) {
        this.id = nextRoomId++;
        this.name = name;
        this.maxPlayers = maxPlayers;
        this.players = new ClientConnection[maxPlayers];
        this.ready = new boolean[maxPlayers];
    }

    /**
     * Add a player to the first available slot. Returns slot index or -1 if full.
     */
    public synchronized int addPlayer(ClientConnection client) {
        if (inGame) return -1;
        for (int i = 0; i < maxPlayers; i++) {
            if (players[i] == null) {
                players[i] = client;
                ready[i] = false;
                connectedCount++;
                return i;
            }
        }
        return -1;
    }

    public synchronized void removePlayer(int slot) {
        if (slot >= 0 && slot < maxPlayers && players[slot] != null) {
            players[slot] = null;
            ready[slot] = false;
            connectedCount--;
        }
    }

    public synchronized boolean setReady(int slot) {
        if (slot >= 0 && slot < maxPlayers) {
            ready[slot] = true;
        }
        boolean allReady = connectedCount > 0;
        for (int i = 0; i < maxPlayers; i++) {
            if (players[i] != null && !ready[i]) {
                allReady = false;
                break;
            }
        }
        return allReady;
    }

    public void broadcastExcept(int senderSlot, NetLibPacket packet) {
        for (int i = 0; i < maxPlayers; i++) {
            if (i != senderSlot && players[i] != null) {
                players[i].queueOutgoing(packet);
            }
        }
    }

    public void broadcastAll(NetLibPacket packet) {
        for (int i = 0; i < maxPlayers; i++) {
            if (players[i] != null) {
                players[i].queueOutgoing(packet);
            }
        }
    }

    public void sendTo(int slot, NetLibPacket packet) {
        if (slot >= 0 && slot < maxPlayers && players[slot] != null) {
            players[slot].queueOutgoing(packet);
        }
    }

    public int getPlayerCount() { return connectedCount; }
    public boolean isFull() { return connectedCount >= maxPlayers; }
    public boolean isEmpty() { return connectedCount == 0; }

    /**
     * Serialize room info for PKTID_ROOM_LIST.
     * Format: [id:2][name_len:1][name:var][cur:1][max:1][in_game:1]
     */
    public byte[] toListEntry() {
        try {
            byte[] nameBytes = name.getBytes("UTF-8");
            if (nameBytes.length > 19) {
                byte[] trimmed = new byte[19];
                System.arraycopy(nameBytes, 0, trimmed, 0, 19);
                nameBytes = trimmed;
            }
            ByteBuffer buf = ByteBuffer.allocate(2 + 1 + nameBytes.length + 3);
            buf.putShort((short) id);
            buf.put((byte) nameBytes.length);
            buf.put(nameBytes);
            buf.put((byte) connectedCount);
            buf.put((byte) maxPlayers);
            buf.put((byte) (inGame ? 1 : 0));
            return buf.array();
        } catch (Exception e) {
            return new byte[0];
        }
    }
}
