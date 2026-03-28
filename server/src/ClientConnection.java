/**
 * ClientConnection — Per-client thread handling packet I/O.
 *
 * Now supports room-based matchmaking. Clients connect, browse rooms,
 * create/join rooms, then play. Game-agnostic — all game logic is
 * in GamePacketHandler.
 */

import java.io.*;
import java.net.*;
import java.nio.*;
import java.util.*;
import java.util.concurrent.*;
import NetLib.*;

public class ClientConnection extends Thread {

    private static final long TIMEOUT = 15000;

    private final DatagramSocket socket;
    private final InetAddress address;
    private final int port;
    private final GameSession session;

    private final ConcurrentLinkedQueue<byte[]> incomingQueue = new ConcurrentLinkedQueue<>();
    private final ConcurrentLinkedQueue<NetLibPacket> outgoingQueue = new ConcurrentLinkedQueue<>();

    private UDPHandler handler;
    private volatile long lastMessageTime;
    private volatile boolean running = true;
    private volatile boolean connected = false;

    // Room association
    private volatile Room room = null;
    private volatile int roomSlot = -1;

    public ClientConnection(DatagramSocket socket, InetAddress address, int port, GameSession session) {
        this.socket = socket;
        this.address = address;
        this.port = port;
        this.session = session;
        this.lastMessageTime = System.currentTimeMillis();
        setDaemon(true);
    }

    public void queueMessage(byte[] data, int length) {
        byte[] copy = new byte[length];
        System.arraycopy(data, 0, copy, 0, length);
        incomingQueue.add(copy);
        lastMessageTime = System.currentTimeMillis();
    }

    public void queueOutgoing(NetLibPacket packet) {
        outgoingQueue.add(packet);
    }

    public Room getRoom() { return room; }
    public int getRoomSlot() { return roomSlot; }

    public void setRoom(Room room, int slot) {
        this.room = room;
        this.roomSlot = slot;
    }

    @Override
    public void run() {
        handler = new UDPHandler(socket, address.getHostAddress(), port);

        try {
            while (running) {
                byte[] data = incomingQueue.poll();
                if (data != null) {
                    processIncoming(data);
                }

                NetLibPacket outPkt = outgoingQueue.poll();
                while (outPkt != null) {
                    handler.SendPacket(outPkt);
                    outPkt = outgoingQueue.poll();
                }

                handler.ResendMissingPackets();

                if (System.currentTimeMillis() - lastMessageTime > TIMEOUT) {
                    System.out.println("Client timed out");
                    break;
                }

                if (incomingQueue.isEmpty() && outgoingQueue.isEmpty()) {
                    Thread.sleep(5);
                }
            }
        } catch (ClientTimeoutException e) {
            System.out.println("Client connection lost");
        } catch (Exception e) {
            System.err.println("Client error: " + e.getMessage());
        } finally {
            GamePacketHandler.handleDisconnect(this);
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
            handler.SendPacket(new S64Packet("DISCOVER", response, PacketFlag.FLAG_UNRELIABLE.GetInt()));
        }
    }

    private void handleNetLib(byte[] data) throws Exception {
        NetLibPacket pkt = handler.ReadNetLibPacket(data);
        if (pkt == null) return;

        if (!connected && pkt.GetType() == GamePacketHandler.PKTID_CONNECT) {
            connected = true;
            GamePacketHandler.handleConnect(this);
            return;
        }

        if (connected) {
            GamePacketHandler.handlePacket(this, pkt);
        }
    }
}
