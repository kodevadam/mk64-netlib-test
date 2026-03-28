/**
 * N64 Netplay Server — Reusable for any N64 decomp game.
 *
 * Routes controller inputs between N64 consoles connected via flashcart USB.
 * Works with SC64, 64Drive, and EverDrive through N64-NetLib.
 *
 * Usage: java N64NetplayServer [--port PORT] [--max-players N] [--name NAME]
 *                               [--master] [--rom ROM_PATH]
 *
 * To adapt for a different game, only GamePacketHandler needs to change.
 * The server core (this file + ClientConnection + NetLib/) is game-agnostic.
 */

import java.io.*;
import java.net.*;
import java.nio.*;
import java.security.*;
import java.util.*;
import NetLib.*;

public class N64NetplayServer {

    // Server configuration
    static int port = 6464;
    static int maxPlayers = 4;
    static String serverName = "N64 Netplay";
    static String romPath = null;
    static byte[] romHash = null;
    static boolean useMaster = false;
    static String masterAddress = "master.n64brew.dev";
    static int masterPort = 6464;

    // Server state
    static DatagramSocket socket;
    static Hashtable<String, ClientConnection> clients = new Hashtable<>();
    static GameSession session = new GameSession();

    public static void main(String[] args) throws Exception {
        readArguments(args);

        System.out.println("=== N64 Netplay Server ===");
        System.out.println("Port: " + port);
        System.out.println("Max players: " + maxPlayers);
        System.out.println("Server name: " + serverName);

        if (romPath != null) {
            romHash = generateRomHash(romPath);
            System.out.println("ROM: " + romPath);
        }

        socket = new DatagramSocket(port);
        System.out.println("Listening on UDP port " + port + "...");

        // Optional: register with master server for discovery
        if (useMaster && romPath != null) {
            MasterConnection master = new MasterConnection(
                socket, masterAddress, masterPort, port, serverName, romHash
            );
            master.start();
            System.out.println("Registering with master server at " + masterAddress);
        }

        // Main receive loop
        byte[] buffer = new byte[4096];
        while (true) {
            DatagramPacket udpPacket = new DatagramPacket(buffer, buffer.length);
            socket.receive(udpPacket);

            String clientKey = udpPacket.getAddress().getHostAddress() + ":" + udpPacket.getPort();

            // Clean up dead connections
            Iterator<Map.Entry<String, ClientConnection>> it = clients.entrySet().iterator();
            while (it.hasNext()) {
                Map.Entry<String, ClientConnection> entry = it.next();
                if (!entry.getValue().isAlive()) {
                    it.remove();
                }
            }

            // Handle S64 discovery packets
            if (S64Packet.IsS64PacketHeader(buffer)) {
                ClientConnection conn = clients.get(clientKey);
                if (conn == null) {
                    conn = new ClientConnection(
                        socket, udpPacket.getAddress(), udpPacket.getPort(), session
                    );
                    conn.start();
                    clients.put(clientKey, conn);
                }
                conn.queueMessage(buffer, udpPacket.getLength());
                continue;
            }

            // Handle NetLib game packets
            if (NetLibPacket.IsNetLibPacketHeader(buffer)) {
                ClientConnection conn = clients.get(clientKey);
                if (conn == null) {
                    conn = new ClientConnection(
                        socket, udpPacket.getAddress(), udpPacket.getPort(), session
                    );
                    conn.start();
                    clients.put(clientKey, conn);
                }
                conn.queueMessage(buffer, udpPacket.getLength());
            }
        }
    }

    static void readArguments(String[] args) {
        for (int i = 0; i < args.length; i++) {
            switch (args[i]) {
                case "--port":
                case "-p":
                    port = Integer.parseInt(args[++i]);
                    break;
                case "--max-players":
                case "-n":
                    maxPlayers = Integer.parseInt(args[++i]);
                    break;
                case "--name":
                    serverName = args[++i];
                    break;
                case "--rom":
                    romPath = args[++i];
                    break;
                case "--master":
                    useMaster = true;
                    break;
                case "--master-address":
                    masterAddress = args[++i];
                    break;
                case "--help":
                case "-h":
                    System.out.println("Usage: java N64NetplayServer [options]");
                    System.out.println("  --port PORT          UDP port (default 6464)");
                    System.out.println("  --max-players N      Max players (default 4)");
                    System.out.println("  --name NAME          Server name");
                    System.out.println("  --rom ROM_PATH       ROM file for hash verification");
                    System.out.println("  --master             Register with master server");
                    System.out.println("  --master-address ADDR  Master server address");
                    System.exit(0);
                    break;
            }
        }
    }

    static byte[] generateRomHash(String path) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        try (FileInputStream fis = new FileInputStream(path)) {
            byte[] buf = new byte[8192];
            int n;
            while ((n = fis.read(buf)) != -1) {
                md.update(buf, 0, n);
            }
        }
        return md.digest();
    }

    /**
     * Build discovery response for S64 DISCOVER packets.
     * Format: [id_len:4][id:var][name_len:4][name:var][players:4][maxplayers:4]
     */
    static byte[] buildDiscoverResponse(String identifier) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] idBytes = identifier.getBytes("UTF-8");
        byte[] nameBytes = serverName.getBytes("UTF-8");

        out.write(ByteBuffer.allocate(4).putInt(idBytes.length).array());
        out.write(idBytes);
        out.write(ByteBuffer.allocate(4).putInt(nameBytes.length).array());
        out.write(nameBytes);
        out.write(ByteBuffer.allocate(4).putInt(session.getPlayerCount()).array());
        out.write(ByteBuffer.allocate(4).putInt(maxPlayers).array());

        return out.toByteArray();
    }
}
