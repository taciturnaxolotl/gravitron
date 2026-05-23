const express = require("express");
const http = require("http");
const { WebSocketServer } = require("ws");
const path = require("path");

const app = express();
const server = http.createServer(app);
const wss = new WebSocketServer({ server });

const AUTH_TOKEN = process.env.AUTH_TOKEN || "swordfish";

app.use(express.json());
app.use(express.static(path.join(__dirname, "public")));

app.post("/authorize", (req, res) => {
	if (req.body.token === AUTH_TOKEN) {
		return res.json({ ok: true });
	}
	res.status(401).json({ ok: false });
});

app.get("/", (req, res) => res.redirect("/controller"));
app.get("/controller", (req, res) => res.sendFile(path.join(__dirname, "public", "controller.html")));
app.get("/operator", (req, res) => res.sendFile(path.join(__dirname, "public", "operator.html")));

// State
let phone = null;        // phone camera WS
let operator = null;     // machine with robot plugged in
let drivers = new Set(); // all connected driver WS connections
let lockHolder = null;   // which driver ws holds the drive lock
let streamDriver = null; // which driver is currently receiving the video stream

function broadcast(clients, msg) {
	const data = JSON.stringify(msg);
	for (const c of clients) {
		if (c.readyState === 1) c.send(data);
	}
}

function driverCount() { return drivers.size; }

function notifyOperator() {
	if (!operator || operator.readyState !== 1) return;
	operator.send(JSON.stringify({
		type: "status",
		drivers: driverCount(),
		locked: lockHolder !== null,
	}));
}

function notifyAllDrivers() {
	for (const d of drivers) {
		if (d.readyState !== 1) continue;
		d.send(JSON.stringify({
			type: "driver-status",
			drivers: driverCount(),
			hasLock: d === lockHolder,
			locked: lockHolder !== null,
		}));
	}
}

wss.on("connection", (ws, req) => {
	const params = new URL(req.url, "http://localhost").searchParams;
	const role = params.get("role");

	// ── Phone ──────────────────────────────────────────────────────────────
	if (role === "phone") {
		phone = ws;
		console.log("Phone connected");
		broadcast(drivers, { type: "phone-joined" });

		ws.on("message", (data) => {
			// phone sends WebRTC signaling only back to the stream driver
			if (streamDriver && streamDriver.readyState === 1) {
				streamDriver.send(data.toString());
			}
		});

		ws.on("close", () => {
			phone = null;
			console.log("Phone disconnected");
			broadcast(drivers, { type: "phone-left" });
		});
		return;
	}

	// ── Operator ───────────────────────────────────────────────────────────
	if (role === "operator") {
		operator = ws;
		console.log("Operator connected");
		notifyOperator();

		ws.on("message", (data) => {
			const msg = JSON.parse(data.toString());
			// Force-release the lock
			if (msg.type === "force-release") {
				if (lockHolder) {
					lockHolder.send(JSON.stringify({ type: "lock-revoked" }));
					lockHolder = null;
					console.log("Lock force-released by operator");
					notifyAllDrivers();
					notifyOperator();
				}
				return;
			}
			// operator echoes serial ack bytes back to lock holder
			if (lockHolder && lockHolder.readyState === 1) {
				lockHolder.send(data.toString());
			}
		});

		ws.on("close", () => {
			operator = null;
			console.log("Operator disconnected");
			broadcast(drivers, { type: "operator-left" });
		});
		return;
	}

	// ── Driver (default) ───────────────────────────────────────────────────
	drivers.add(ws);
	console.log(`Driver connected (${driverCount()} total)`);
	notifyOperator();
	notifyAllDrivers();

	// Send current phone state
	if (phone) ws.send(JSON.stringify({ type: "phone-joined" }));

	ws.on("message", (data) => {
		const msg = JSON.parse(data.toString());

		// WebRTC signaling — forward to phone, track which driver owns the stream
		if (["offer", "answer", "candidate", "request-stream"].includes(msg.type)) {
			streamDriver = ws;
			if (phone && phone.readyState === 1) phone.send(data.toString());
			return;
		}

		// Torch — forward to phone
		if (msg.type === "torch") {
			if (phone && phone.readyState === 1) phone.send(data.toString());
			return;
		}

		// Lock acquire
		if (msg.type === "acquire-lock") {
			if (lockHolder === null) {
				lockHolder = ws;
				console.log("Lock acquired");
				notifyAllDrivers();
				notifyOperator();
			} else {
				ws.send(JSON.stringify({ type: "lock-denied" }));
			}
			return;
		}

		// Lock release
		if (msg.type === "release-lock") {
			if (lockHolder === ws) {
				lockHolder = null;
				console.log("Lock released");
				notifyAllDrivers();
				notifyOperator();
			}
			return;
		}

		// Control frame — only from lock holder, forward to operator
		if (msg.type === "frame") {
			if (ws === lockHolder && operator && operator.readyState === 1) {
				// send raw bytes if provided, else forward JSON for operator to write
				operator.send(data.toString());
			}
			return;
		}
	});

	ws.on("close", () => {
		drivers.delete(ws);
		if (lockHolder === ws) {
			lockHolder = null;
			console.log("Lock holder disconnected — lock released");
			notifyAllDrivers();
		}
		if (streamDriver === ws) {
			streamDriver = null;
		}
		console.log(`Driver disconnected (${driverCount()} remaining)`);
		notifyOperator();
		notifyAllDrivers();
	});
});

const PORT = process.env.PORT || 3000;
server.listen(PORT, "0.0.0.0", () => {
	const nets = require("os").networkInterfaces();
	let localIP = "localhost";
	for (const name of Object.keys(nets)) {
		for (const net of nets[name]) {
			if (net.family === "IPv4" && !net.internal) {
				localIP = net.address;
				break;
			}
		}
	}
	console.log(`\nServer running on port ${PORT}`);
	console.log(`  Controller: http://localhost:${PORT}/controller`);
	console.log(`  Operator:   http://localhost:${PORT}/operator`);
	console.log(`  Network:    http://${localIP}:${PORT}`);
	console.log(`\nUse ngrok for remote access:`);
	console.log(`  ngrok http ${PORT}\n`);
});
