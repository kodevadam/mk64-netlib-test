/**
 * GameSession — Manages connected players and game state.
 *
 * This is the game-agnostic session layer. It tracks which players
 * are connected, their slot assignments, and ready state. Game-specific
 * config (characters, course, mode) is handled by GamePacketHandler.
 *
 * Reusable across any N64 decomp — no game-specific logic here.
 */

import java.nio.*;
import java.util.*;
import java.util.concurrent.*;
import NetLib.*;

public class GameSession {

    private final PlayerSlot[] players;
    private final int maxPlayers;
    private volatile int connectedCount = 0;
    private volatile boolean allReady = false;

    // Game config (set by host player, forwarded to all)
    private volatile byte[] gameConfig = null;

    public GameSession() {
        this(N64NetplayServer.maxPlayers);
    }

    public GameSession(int maxPlayers) {
        this.maxPlayers = maxPlayers;
        this.players = new PlayerSlot[maxPlayers];
        for (int i = 0; i < maxPlayers; i++) {
            players[i] = new PlayerSlot(i);
        }
    }

    /**
     * Assign a new player to the first available slot.
     * Returns the slot index (0-based), or -1 if full.
     */
    public synchronized int connectPlayer(ClientConnection client) {
        for (int i = 0; i < maxPlayers; i++) {
            if (players[i].client == null) {
                players[i].client = client;
                players[i].ready = false;
                connectedCount++;
                System.out.println("Player " + i + " connected (" + connectedCount + "/" + maxPlayers + ")");
                return i;
            }
        }
        return -1; // Server full
    }

    /**
     * Remove a player from their slot.
     */
    public synchronized void disconnectPlayer(int slot) {
        if (slot >= 0 && slot < maxPlayers && players[slot].client != null) {
            players[slot].client = null;
            players[slot].ready = false;
            connectedCount--;
            allReady = false;
            System.out.println("Player " + slot + " disconnected (" + connectedCount + "/" + maxPlayers + ")");
        }
    }

    /**
     * Mark a player as ready for race start.
     * Returns true if ALL connected players are now ready.
     */
    public synchronized boolean setPlayerReady(int slot) {
        if (slot >= 0 && slot < maxPlayers) {
            players[slot].ready = true;
        }

        // Check if all connected players are ready
        boolean ready = connectedCount > 0;
        for (int i = 0; i < maxPlayers; i++) {
            if (players[i].client != null && !players[i].ready) {
                ready = false;
                break;
            }
        }
        allReady = ready;
        return allReady;
    }

    public boolean isAllReady() {
        return allReady;
    }

    /**
     * Store game config from host player and forward to all.
     */
    public void setGameConfig(byte[] config) {
        this.gameConfig = config;
    }

    public byte[] getGameConfig() {
        return gameConfig;
    }

    /**
     * Send a packet to all connected players except the sender.
     */
    public void broadcastExcept(int senderSlot, NetLibPacket packet) {
        for (int i = 0; i < maxPlayers; i++) {
            if (i != senderSlot && players[i].client != null) {
                players[i].client.queueOutgoing(packet);
            }
        }
    }

    /**
     * Send a packet to all connected players.
     */
    public void broadcastAll(NetLibPacket packet) {
        for (int i = 0; i < maxPlayers; i++) {
            if (players[i].client != null) {
                players[i].client.queueOutgoing(packet);
            }
        }
    }

    /**
     * Send a packet to a specific player slot.
     */
    public void sendTo(int slot, NetLibPacket packet) {
        if (slot >= 0 && slot < maxPlayers && players[slot].client != null) {
            players[slot].client.queueOutgoing(packet);
        }
    }

    public int getPlayerCount() {
        return connectedCount;
    }

    public int getMaxPlayers() {
        return maxPlayers;
    }

    public PlayerSlot[] getPlayers() {
        return players;
    }

    /**
     * Simple container for a player slot.
     */
    public static class PlayerSlot {
        public final int index;
        public volatile ClientConnection client;
        public volatile boolean ready;

        public PlayerSlot(int index) {
            this.index = index;
            this.client = null;
            this.ready = false;
        }
    }
}
