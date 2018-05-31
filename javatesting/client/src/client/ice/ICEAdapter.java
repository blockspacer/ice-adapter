package client.ice;

import client.GUI;
import client.TestClient;
import com.github.nocatch.NoCatch;
import com.google.gson.Gson;
import com.nbarraille.jjsonrpc.JJsonPeer;
import com.nbarraille.jjsonrpc.TcpClient;
import data.IceStatus;
import javafx.beans.property.BooleanProperty;
import javafx.beans.property.SimpleBooleanProperty;
import javafx.scene.control.Alert;
import logging.Logger;
import util.Util;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;
import java.io.File;
import java.io.IOException;
import java.net.ConnectException;
import java.nio.charset.Charset;
import java.security.InvalidKeyException;
import java.security.NoSuchAlgorithmException;
import java.util.*;

public class ICEAdapter {

	private static final String LOG_LEVEL = "error";

	private static final int CONNECTION_ATTEMPTS = 5;

	private static final String COTURN_HOST = "vmrbg145.informatik.tu-muenchen.de";
	private static final String COTURN_KEY = "banana";

	public static int ADAPTER_PORT;//RPC Client <=> ICE
	public static int GPG_PORT;//ICE <=> Lobby
	public static int LOBBY_PORT;//forgedalliance udp receive point

	private static Process process;
	private static TcpClient tcpClient;
	private static JJsonPeer peer;

	public static BooleanProperty connected = new SimpleBooleanProperty(false);

	private static IceAdapterCallbacks iceAdapterCallbacks = new IceAdapterCallbacks();

	public static void init() {
		Alert alert = NoCatch.noCatch(() -> GUI.showDialog("Starting ICE adapter...").get());
		startICEAdapter();

		try { Thread.sleep(500); } catch(InterruptedException e) {}

		connectToICEAdapter();
		configureIceServers();

		connected.set(true);

		GUI.runAndWait(alert::close);

		try { Thread.sleep(500); } catch(InterruptedException e) {}
	}


	public static void hostGame(String mapName) {
		peer.sendAsyncRequest("hostGame", Arrays.asList(mapName), null, true);
	}

	public static void joinGame(String remotePlayerLogin, long remotePlayerId) {
		peer.sendAsyncRequest("joinGame", Arrays.asList(remotePlayerLogin, remotePlayerId), null, true);
	}
	public static void connectToPeer(String remotePlayerLogin, long remotePlayerId, boolean offer) {
		peer.sendAsyncRequest("connectToPeer", Arrays.asList(remotePlayerLogin, remotePlayerId, offer), null, true);
	}

	public static void disconnectFromPeer(long remotePlayerId) {
		peer.sendAsyncRequest("disconnectFromPeer", Arrays.asList(remotePlayerId), null, true);
	}

	public static void setLobbyInitMode(String lobbyInitMode) {
		peer.sendAsyncRequest("setLobbyInitMode", Arrays.asList(lobbyInitMode), null, true);
	}

	public static void iceMsg(long remotePlayerId, Object msg) {
		peer.sendAsyncRequest("iceMsg", Arrays.asList(remotePlayerId, msg), null, true);
	}

	public static void sendToGpgNet(String header, String... chunks) {
		peer.sendAsyncRequest("sendToGpgNet", Arrays.asList(header, chunks), null, true);
	}

	public static void setIceServers(List<Map<String, Object>> iceServers) {
		peer.sendAsyncRequest("setIceServers", Arrays.asList(iceServers), null, true);
	}

	public static IceStatus status() {
		return new Gson().fromJson((peer.sendSyncRequest("status", Collections.emptyList(), true)).toString(), IceStatus.class);
	}


	private static void configureIceServers() {
		int timestamp = (int)(System.currentTimeMillis() / 1000) + 3600 * 24;
		String tokenName = String.format("%s:%s", timestamp, TestClient.username);
		byte[] secret = null;
		try {
			Mac mac = Mac.getInstance("HmacSHA1");
			mac.init(new SecretKeySpec(Charset.forName("UTF-8").encode(COTURN_KEY).array(), "HmacSHA1"));
			secret = mac.doFinal(Charset.forName("UTF-8").encode(tokenName).array());

		} catch(NoSuchAlgorithmException | InvalidKeyException e) { Logger.crash(e); }
		String authToken = Base64.getEncoder().encodeToString(secret);

		Map<String, Object> map = new HashMap<>();
		map.put("urls", Arrays.asList(
				String.format("turn:%s?transport=tcp", COTURN_HOST),
				String.format("turn:%s?transport=udp", COTURN_HOST),
				String.format("stun:%s", COTURN_HOST)
		));

		map.put("credential", authToken);
		map.put("credentialType", "authToken");
		map.put("username", tokenName);

		setIceServers(Arrays.asList(map));
		Logger.debug("Set ICE servers");
	}

	private static void connectToICEAdapter() {
		for (int attempt = 0; attempt < CONNECTION_ATTEMPTS; attempt++) {
			try {
				tcpClient = new TcpClient("localhost", ADAPTER_PORT, iceAdapterCallbacks);
				peer = tcpClient.getPeer();

//				setIceServers();
//				setLobbyInitMode();
				break;
			} catch (ConnectException e) {
				Logger.debug("Could not connect to ICE adapter (attempt %s/%s)", attempt, CONNECTION_ATTEMPTS);
			} catch (IOException e) {
				Logger.crash(e);
			}
		}

		if(peer == null) {
			Logger.error("Could not connect to ICE adapter.");
			System.exit(45);
		}

		Logger.debug("Connected to ICE client via JsonRPC.");
	}


	public static void startICEAdapter() {
		Logger.info("Launching ICE adapter...");

		ADAPTER_PORT = Util.getAvaiableTCPPort();
		GPG_PORT = Util.getAvaiableTCPPort();
		LOBBY_PORT = Util.getAvaiableTCPPort();

		if (ADAPTER_PORT == GPG_PORT) {
			Logger.crash(new RuntimeException("ADAPTER_PORT = GPG_PORT, you got very unlucky, this is NO error, just try again!"));
		}

		String command[] = new String[]{
				(System.getProperty("os.name").contains("Windows") ? "faf-ice-adapter.exe" : "./faf-ice-adapter"),
				"--id", String.valueOf(TestClient.playerID),
				"--login", TestClient.username,
				"--rpc-port", String.valueOf(ADAPTER_PORT),
				"--gpgnet-port", String.valueOf(GPG_PORT),
				"--lobby-port", String.valueOf(LOBBY_PORT),
				"--log-level", LOG_LEVEL
		};

		ProcessBuilder processBuilder = new ProcessBuilder(command);
		processBuilder.inheritIO();
		processBuilder.redirectOutput(new File("faf-ice-adapter.log"));

		Logger.debug("Command: %s", Arrays.stream(command).reduce("", (l, r) -> l + " " + r));
		try {
			process = processBuilder.start();
		} catch (IOException e) {
			Logger.error("Could not start ICE adapter", e);
			System.exit(10);
		}

		if (!process.isAlive()) {
			Logger.error("ICE Adapter not running");
			System.exit(11);
		}

		Logger.info("Launched ICE adapter");
	}

	public static void close() {
		Logger.debug("Closing ICE adapter...");
		if (peer != null && peer.isAlive()) {
			peer.sendAsyncRequest("quit", Collections.emptyList(), null, false);
			try {
				Thread.sleep(500);
			} catch (InterruptedException e) {
			}
		}

		if (process.isAlive()) {
			Logger.debug("ICE adapter running, killing...");
			process.destroyForcibly();
		}

		connected.set(false);
	}
}
