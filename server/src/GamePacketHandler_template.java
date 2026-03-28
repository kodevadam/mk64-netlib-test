/**
 * GamePacketHandler — Game-specific packet routing logic.
 *
 * THIS IS THE ONLY FILE YOU NEED TO MODIFY FOR A DIFFERENT GAME.
 *
 * Packet IDs must match the N64 client (netplay.h).
 * The server's job is simple:
 *   - Assign player slots on connect
 *   - Relay inputs between players
 *   - Forward game config from host to all
 *   - Track ready state and signal race start
 *   - Notify on disconnect
 *
 * For a new decomp game:
 *   1. Copy this file
 *   2. Change packet IDs to match your game's netplay.h
 *   3. Adjust PKTID_SERVER_CONFIG payload if your game config differs
 *   4. Everything else (connect, input relay, ready, disconnect) is generic
 */

import java.nio.*;
import NetLib.*;

public class GamePacketHandler {

    // ---- Packet IDs (must match N64 client netplay.h) ----

    // Client → Server
    public static final int PKTID_CONNECT        = 0x00;
    public static final int PKTID_PLAYER_INPUT    = 0x01;
    public static final int PKTID_READY           = 0x02;
    public static final int PKTID_GAME_CONFIG     = 0x03;
    public static final int PKTID_HEARTBEAT_ACK   = 0x04;

    // Server → Client
    public static final int PKTID_ASSIGN_PLAYER   = 0x10;
    public static final int PKTID_REMOTE_INPUT    = 0x11;
    public static final int PKTID_ALL_READY       = 0x12;
    public static final int PKTID_SERVER_CONFIG   = 0x13;
    public static final int PKTID_PLAYER_LEFT     = 0x14;
    public static final int PKTID_HEARTBEAT       = 0x15;

    /**
     * Called when a new player connects and is assigned a slot.
     * Sends them their player number and current player count.
     */
    public static void handleConnect(GameSession session, int slot) {
        // Send ASSIGN_PLAYER to the new client: [player_num:1][player_count:1]
        byte[] data = new byte[] {
            (byte) slot,
            (byte) session.getPlayerCount()
        };
        session.sendTo(slot, new NetLibPacket(PKTID_ASSIGN_PLAYER, data));

        // Notify existing players of updated count by re-sending their assignment
        for (GameSession.PlayerSlot ps : session.getPlayers()) {
            if (ps.client != null && ps.index != slot) {
                byte[] update = new byte[] {
                    (byte) ps.index,
                    (byte) session.getPlayerCount()
                };
                session.sendTo(ps.index, new NetLibPacket(PKTID_ASSIGN_PLAYER, update));
            }
        }

        // If host already sent game config, forward to new player
        byte[] config = session.getGameConfig();
        if (config != null) {
            session.sendTo(slot, new NetLibPacket(PKTID_SERVER_CONFIG, config));
        }

        System.out.println("Player " + slot + " assigned (total: " + session.getPlayerCount() + ")");
    }

    /**
     * Called when a player disconnects (timeout or leave).
     * Notifies all remaining players.
     */
    public static void handleDisconnect(GameSession session, int slot) {
        byte[] data = new byte[] { (byte) slot };
        session.broadcastAll(new NetLibPacket(PKTID_PLAYER_LEFT, data));
        System.out.println("Notified others: player " + slot + " left");
    }

    /**
     * Route a game packet from a connected player.
     */
    public static void handlePacket(GameSession session, int senderSlot, NetLibPacket pkt) {
        switch (pkt.GetType()) {

            case PKTID_PLAYER_INPUT:
                handlePlayerInput(session, senderSlot, pkt);
                break;

            case PKTID_READY:
                handleReady(session, senderSlot);
                break;

            case PKTID_GAME_CONFIG:
                handleGameConfig(session, senderSlot, pkt);
                break;

            case PKTID_HEARTBEAT_ACK:
                // Client responded to our heartbeat — connection alive
                break;

            default:
                // Unknown packet — relay to recipients if mask is set
                if (pkt.GetRecipients() != 0) {
                    relayToRecipients(session, senderSlot, pkt);
                }
                break;
        }
    }

    /**
     * Relay controller input from one player to all others.
     *
     * Client sends: PKTID_PLAYER_INPUT [frame:4][packed_input:4]
     * Server wraps: PKTID_REMOTE_INPUT [player:1][frame:4][packed_input:4]
     */
    private static void handlePlayerInput(GameSession session, int senderSlot, NetLibPacket pkt) {
        byte[] clientData = pkt.GetData();
        if (clientData == null || clientData.length < 8) return;

        // Prepend sender's player slot number
        byte[] relayData = new byte[1 + clientData.length];
        relayData[0] = (byte) senderSlot;
        System.arraycopy(clientData, 0, relayData, 1, clientData.length);

        NetLibPacket relay = new NetLibPacket(PKTID_REMOTE_INPUT, relayData, PacketFlag.FLAG_UNRELIABLE);
        session.broadcastExcept(senderSlot, relay);
    }

    /**
     * Player signals ready for race start.
     * When all connected players are ready, broadcast ALL_READY.
     */
    private static void handleReady(GameSession session, int senderSlot) {
        System.out.println("Player " + senderSlot + " ready");

        if (session.setPlayerReady(senderSlot)) {
            System.out.println("All players ready — starting race!");
            session.broadcastAll(new NetLibPacket(PKTID_ALL_READY, null));
        }
    }

    /**
     * Host player (slot 0) sends game configuration.
     * Server stores it and forwards to all players.
     *
     * Payload: [mode:1][course:2][cc:1][char0:1][char1:1][char2:1][char3:1][rng_seed:4][input_delay:1]
     * Total: 13 bytes (for 4-player game)
     */
    private static void handleGameConfig(GameSession session, int senderSlot, NetLibPacket pkt) {
        byte[] config = pkt.GetData();
        if (config == null) return;

        // Only player 0 (host) can set game config
        if (senderSlot != 0) {
            System.out.println("Warning: non-host player " + senderSlot + " tried to set config");
            return;
        }

        session.setGameConfig(config);
        session.broadcastAll(new NetLibPacket(PKTID_SERVER_CONFIG, config));
        System.out.println("Game config set by host and forwarded to all players");
    }

    /**
     * Generic relay: forward packet to players specified in recipient mask.
     */
    private static void relayToRecipients(GameSession session, int senderSlot, NetLibPacket pkt) {
        int mask = pkt.GetRecipients();
        for (GameSession.PlayerSlot ps : session.getPlayers()) {
            if (ps.client != null && ps.index != senderSlot) {
                // NetLib recipients use 1-indexed bitmask (bit N = client N+1)
                // But we use 0-indexed slots, so check both conventions
                if ((mask & (1 << ps.index)) != 0 || (mask & (1 << (ps.index + 1))) != 0) {
                    ps.client.queueOutgoing(pkt);
                }
            }
        }
    }
}
