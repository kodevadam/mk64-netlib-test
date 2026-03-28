/**
 * MasterConnection — Registers this server with the N64-NetLib master server
 * at master.n64brew.dev so it appears in the server browser.
 *
 * Sends REGISTER on startup, then HEARTBEAT every 3 minutes.
 * Reusable — no game-specific logic.
 */

import java.io.*;
import java.net.*;
import java.nio.*;
import java.util.concurrent.*;
import NetLib.*;

public class MasterConnection extends Thread {

    private static final long HEARTBEAT_INTERVAL = 1000 * 60 * 3; // 3 minutes

    private final DatagramSocket socket;
    private final InetAddress masterAddress;
    private final int masterPort;
    private final int serverPort;
    private final String serverName;
    private final byte[] romHash;

    private final ConcurrentLinkedQueue<byte[]> msgQueue = new ConcurrentLinkedQueue<>();
    private UDPHandler handler;

    public MasterConnection(DatagramSocket socket, String masterHost, int masterPort,
                            int serverPort, String serverName, byte[] romHash) throws Exception {
        this.socket = socket;
        this.masterAddress = InetAddress.getByName(masterHost);
        this.masterPort = masterPort;
        this.serverPort = serverPort;
        this.serverName = serverName;
        this.romHash = romHash;
        setDaemon(true);
    }

    public void queueMessage(byte[] data, int length) {
        byte[] copy = new byte[length];
        System.arraycopy(data, 0, copy, 0, length);
        msgQueue.add(copy);
    }

    @Override
    public void run() {
        handler = new UDPHandler(socket, masterAddress, masterPort);

        try {
            // Initial registration
            sendRegister();
            waitForAck();

            // Periodic heartbeat
            while (true) {
                Thread.sleep(HEARTBEAT_INTERVAL);
                sendHeartbeat();
                waitForAck();
            }
        } catch (Exception e) {
            System.err.println("Master server connection error: " + e.getMessage());
        }
    }

    private void sendRegister() throws Exception {
        handler.SendPacket(new S64Packet("REGISTER", buildMasterData(), PacketFlag.FLAG_EXPLICITACK));
    }

    private void sendHeartbeat() throws Exception {
        handler.SendPacket(new S64Packet("HEARTBEAT", buildMasterData(), PacketFlag.FLAG_EXPLICITACK));
    }

    private byte[] buildMasterData() throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] nameBytes = serverName.getBytes("UTF-8");

        out.write(ByteBuffer.allocate(4).putInt(serverPort).array());
        out.write(ByteBuffer.allocate(4).putInt(nameBytes.length).array());
        out.write(nameBytes);
        if (romHash != null) {
            out.write(ByteBuffer.allocate(4).putInt(romHash.length).array());
            out.write(romHash);
        } else {
            out.write(ByteBuffer.allocate(4).putInt(0).array());
        }

        return out.toByteArray();
    }

    private void waitForAck() throws Exception {
        long start = System.currentTimeMillis();
        while (System.currentTimeMillis() - start < 10000) { // 10s timeout
            byte[] reply = msgQueue.poll();
            if (reply != null && S64Packet.IsS64PacketHeader(reply)) {
                S64Packet pkt = handler.ReadS64Packet(reply);
                if (pkt != null && pkt.GetType().equals("ACK")) {
                    return;
                }
            }
            handler.ResendMissingPackets();
            Thread.sleep(1000);
        }
        System.err.println("Warning: master server ack timeout");
    }
}
