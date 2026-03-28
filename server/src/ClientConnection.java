/**
 * ClientConnection — Per-client thread handling packet I/O.
 *
 * Each connected N64 gets its own thread. The thread:
 * 1. Receives packets from the main loop via message queue
 * 2. Parses them through the UDPHandler (reliability layer)
 * 3. Routes them through GamePacketHandler (game logic)
 * 4. Sends outgoing packets queued by the game session
 *
 * Reusable across any N64 decomp. Game-specific logic is in
 * GamePacketHandler, which this class delegates to.
 */

import java.io.*;
import java.net.*;
import java.nio.*;
import java.util.*;
import java.util.concurrent.*;
import NetLib.*;

public class ClientConnection extends Thread {

    private static final long HEARTBEAT_INTERVAL = 5000; // 5 seconds
    private static final long TIMEOUT = 15000; // 15 seconds no message = disconnect

    private final DatagramSocket socket;
    private final InetAddress address;
    private final int port;
    private final GameSession session;

    // Incoming messages from main receive loop
    private final ConcurrentLinkedQueue<byte[]> incomingQueue = new ConcurrentLinkedQueue<>();
    // Outgoing messages to send to this client
    private final ConcurrentLinkedQueue<NetLibPacket> outgoingQueue = new ConcurrentLinkedQueue<>();

    private UDPHandler handler;
    private int playerSlot = -1;
    private volatile long lastMessageTime;
    private volatile boolean running = true;

    public ClientConnection(DatagramSocket socket, InetAddress address, int port, GameSession session) {
        this.socket = socket;
        this.address = address;
        this.port = port;
        this.session = session;
        this.lastMessageTime = System.currentTimeMillis();
        setDaemon(true);
    }

    /**
     * Called by main thread to deliver a raw UDP packet to this client's queue.
     */
    public void queueMessage(byte[] data, int length) {
        byte[] copy = new byte[length];
        System.arraycopy(data, 0, copy, 0, length);
        incomingQueue.add(copy);
        lastMessageTime = System.currentTimeMillis();
    }

    /**
     * Called by GameSession to queue an outgoing packet.
     */
    public void queueOutgoing(NetLibPacket packet) {
        outgoingQueue.add(packet);
    }

    public int getPlayerSlot() {
        return playerSlot;
    }

    @Override
    public void run() {
        handler = new UDPHandler(socket, address, port);

        try {
            while (running) {
                // Process incoming messages
                byte[] data = incomingQueue.poll();
                if (data != null) {
                    processIncoming(data);
                }

                // Send outgoing packets
                NetLibPacket outPkt = outgoingQueue.poll();
                while (outPkt != null) {
                    handler.SendPacket(outPkt);
                    outPkt = outgoingQueue.poll();
                }

                // Resend unacked reliable packets
                handler.ResendMissingPackets();

                // Check for timeout
                if (System.currentTimeMillis() - lastMessageTime > TIMEOUT) {
                    System.out.println("Client " + address + ":" + port + " timed out");
                    break;
                }

                // Don't spin — brief sleep when idle
                if (incomingQueue.isEmpty() && outgoingQueue.isEmpty()) {
                    Thread.sleep(5);
                }
            }
        } catch (ClientTimeoutException e) {
            System.out.println("Client " + address + ":" + port + " connection lost");
        } catch (Exception e) {
            System.err.println("Client error: " + e.getMessage());
        } finally {
            // Clean up: notify other players of disconnect
            if (playerSlot >= 0) {
                session.disconnectPlayer(playerSlot);
                GamePacketHandler.handleDisconnect(session, playerSlot);
            }
            running = false;
        }
    }

    private void processIncoming(byte[] data) throws Exception {
        if (S64Packet.IsS64PacketHeader(data)) {
            handleS64(data);
        } else if (NetLibPacket.IsNetLibPacketHeader(data)) {
            handleNetLib(data);
        }
    }

    private void handleS64(byte[] data) throws Exception {
        S64Packet pkt = handler.ReadS64Packet(data);
        if (pkt == null) return;

        if (pkt.GetType().equals("DISCOVER")) {
            String identifier = new String(pkt.GetData(), "UTF-8");
            byte[] response = N64NetplayServer.buildDiscoverResponse(identifier);
            handler.SendPacket(new S64Packet("DISCOVER", response, PacketFlag.FLAG_UNRELIABLE));
        }
    }

    private void handleNetLib(byte[] data) throws Exception {
        NetLibPacket pkt = handler.ReadNetLibPacket(data);
        if (pkt == null) return;

        // If not yet assigned a player slot, handle connection
        if (playerSlot < 0) {
            if (pkt.GetType() == GamePacketHandler.PKTID_CONNECT) {
                playerSlot = session.connectPlayer(this);
                if (playerSlot < 0) {
                    // Server full — could send a rejection packet here
                    System.out.println("Rejected connection: server full");
                    running = false;
                    return;
                }
                GamePacketHandler.handleConnect(session, playerSlot);
            }
            return;
        }

        // Connected — delegate to game-specific handler
        GamePacketHandler.handlePacket(session, playerSlot, pkt);
    }
}
