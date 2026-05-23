const express = require("express");
const http = require("http");
const { WebSocketServer } = require("ws");
const path = require("path");
require("dotenv").config();

const app = express();
const server = http.createServer(app);
const wss = new WebSocketServer({ server });

const AUTH_TOKEN = process.env.AUTH_TOKEN || "swordfish";
const CF_APP_ID  = process.env.CF_APP_ID;
const CF_TOKEN   = process.env.CF_TOKEN;
const CF_BASE    = `https://rtc.live.cloudflare.com/v1/apps/${CF_APP_ID}`;

async function cfFetch(path, method = "GET", body = null) {
	const opts = {
		method,
		headers: {
			Authorization: `Bearer ${CF_TOKEN}`,
			"Content-Type": "application/json",
		},
	};
	if (body) opts.body = JSON.stringify(body);
	const res = await fetch(`${CF_BASE}${path}`, opts);
	return res.json();
}

app.use(express.json());
app.use(express.static(path.join(__dirname, "public")));

app.post("/authorize", (req, res) => {
	if (req.body.token === AUTH_TOKEN) return res.json({ ok: true });
	res.status(401).json({ ok: false });
});

app.get("/", (req, res) => res.redirect("/controller"));
app.get("/controller", (req, res) => res.sendFile(path.join(__dirname, "public", "controller.html")));
app.get("/operator",   (req, res) => res.sendFile(path.join(__dirname, "public", "operator.html")));

// ── Cloudflare Calls API proxy ─────────────────────────────────────────────
// Phone: create a new session
app.post("/cf/session/new", async (req, res) => {
	try {
		const data = await cfFetch("/sessions/new", "POST");
		res.json(data);
	} catch (e) {
		res.status(500).json({ error: e.message });
	}
});

// Phone/Driver: publish or pull tracks on a session
app.post("/cf/session/:sessionId/tracks/new", async (req, res) => {
	try {
		const data = await cfFetch(`/sessions/${req.params.sessionId}/tracks/new`, "POST", req.body);
		res.json(data);
	} catch (e) {
		res.status(500).json({ error: e.message });
	}
});

// Renegotiate (send answer SDP back to CF)
app.put("/cf/session/:sessionId/renegotiate", async (req, res) => {
	try {
		const data = await cfFetch(`/sessions/${req.params.sessionId}/renegotiate`, "PUT", req.body);
		res.json(data);
	} catch (e) {
		res.status(500).json({ error: e.message });
	}
});

// Close tracks
app.put("/cf/session/:sessionId/tracks/close", async (req, res) => {
	try {
		const data = await cfFetch(`/sessions/${req.params.sessionId}/tracks/close`, "PUT", req.body);
		res.json(data);
	} catch (e) {
		res.status(500).json({ error: e.message });
	}
});

// State
let phone = null;
let operator = null;
let drivers = new Map(); // ws → { name, inQueue }
let lockHolder = null;   // ws of current lock holder
let lockQueue = [];      // ordered array of ws waiting for lock
let streamDriver = null;

const INACTIVITY_LIMIT = 30;  // seconds of no frames before revoke
const MAX_HOLD_TIME    = 60;  // seconds max hold when queue is non-empty

let lockGrantedAt   = 0;
let lockLastFrameAt = 0;
let lockTimerInterval = null;

function startLockTimer() {
	stopLockTimer();
	lockGrantedAt   = Date.now();
	lockLastFrameAt = Date.now();
	lockTimerInterval = setInterval(tickLockTimer, 1000);
}

function stopLockTimer() {
	if (lockTimerInterval) { clearInterval(lockTimerInterval); lockTimerInterval = null; }
}

function tickLockTimer() {
	if (!lockHolder) { stopLockTimer(); return; }
	const now = Date.now();
	const inactiveSec = Math.floor((now - lockLastFrameAt) / 1000);
	const heldSec     = Math.floor((now - lockGrantedAt)   / 1000);
	const hasQueue    = lockQueue.length > 0;

	const inactivityLeft = INACTIVITY_LIMIT - inactiveSec;
	const maxHoldLeft    = MAX_HOLD_TIME    - heldSec;
	const timeLeft       = hasQueue ? Math.min(inactivityLeft, maxHoldLeft) : inactivityLeft;

	if (timeLeft <= 0) {
		const name = drivers.get(lockHolder)?.name;
		console.log(`Lock auto-revoked from ${name} (inactivity=${inactiveSec}s held=${heldSec}s queue=${hasQueue})`);
		lockHolder.send(JSON.stringify({ type: 'lock-revoked', reason: 'timeout' }));
		lockHolder = null;
		stopLockTimer();
		advanceLockQueue();
		if (lockHolder === null) { notifyAllDrivers(); notifyOperator(); }
		return;
	}

	// Push countdown to lock holder
	if (lockHolder && lockHolder.readyState === 1) {
		lockHolder.send(JSON.stringify({ type: 'lock-countdown', secondsLeft: timeLeft, hasQueue }));
	}
}

const ADJECTIVES = ['swift','brave','calm','dark','eager','fancy','gentle','happy','icy','jolly','kind','lively','merry','neat','odd','proud','quick','rare','shiny','tiny','icy','wary','zany','bold','cool','deft','epic','fast','grim','hazy'];
const NOUNS = ['badger','cobra','dingo','eagle','finch','gecko','heron','ibis','jackal','koala','lemur','mink','newt','otter','panda','quail','raven','stoat','tapir','urial','viper','wren','yak','zorilla','bison','crane','drake','egret'];
let nameCounter = 0;
function genName() {
	const a = ADJECTIVES[nameCounter % ADJECTIVES.length];
	const n = NOUNS[Math.floor(nameCounter / ADJECTIVES.length) % NOUNS.length];
	nameCounter++;
	return a + '-' + n;
}

function broadcast(clients, msg) {
	const data = JSON.stringify(msg);
	for (const c of clients.keys ? clients.keys() : clients) {
		if (c.readyState === 1) c.send(data);
	}
}

function driverCount() { return drivers.size; }

let phoneSessionId = null;
let phoneTracks = [];

function driverInfo(ws) {
	const d = drivers.get(ws);
	if (!d) return null;
	const geo = d.geo;
	return {
		name: d.name,
		ping: d.ping,
		city: geo?.city_name ?? null,
		region: geo?.subdivision_1_iso_code ?? null,
		country: geo?.country_iso_code ?? null,
		flag: geo?.country_iso_code ? countryFlag(geo.country_iso_code) : null,
	};
}

function countryFlag(iso) {
	// Convert ISO 3166-1 alpha-2 to emoji flag
	return [...iso.toUpperCase()].map(c => String.fromCodePoint(0x1F1E6 + c.charCodeAt(0) - 65)).join('');
}

function queueSnapshot() {
	return lockQueue.map(ws => driverInfo(ws)).filter(Boolean);
}

function notifyOperator() {
	if (!operator || operator.readyState !== 1) return;
	const allDrivers = [...drivers.keys()].map((ws, i) => ({
		...driverInfo(ws),
		hasLock: ws === lockHolder,
		queuePos: lockQueue.indexOf(ws),
	}));
	operator.send(JSON.stringify({
		type: "status",
		drivers: driverCount(),
		allDrivers,
		locked: lockHolder !== null,
		lockHolder: lockHolder ? drivers.get(lockHolder)?.name : null,
		queue: queueSnapshot(),
	}));
}

function notifyAllDrivers() {
	for (const [d, info] of drivers) {
		if (d.readyState !== 1) continue;
		const qpos = lockQueue.indexOf(d);
		d.send(JSON.stringify({
			type: "driver-status",
			name: info.name,
			drivers: driverCount(),
			hasLock: d === lockHolder,
			locked: lockHolder !== null,
			lockHolder: lockHolder ? drivers.get(lockHolder)?.name : null,
			queuePos: qpos, // -1 = not in queue
			queue: queueSnapshot(),
		}));
	}
}

function advanceLockQueue() {
	if (lockHolder !== null) return;
	while (lockQueue.length > 0) {
		const next = lockQueue.shift();
		if (next.readyState === 1) {
			lockHolder = next;
			console.log(`Lock auto-granted to ${drivers.get(next)?.name}`);
			startLockTimer();
			notifyAllDrivers();
			notifyOperator();
			return;
		}
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
			const msg = JSON.parse(data.toString());
			if (msg.type === "cf-session") {
				phoneSessionId = msg.sessionId;
				phoneTracks = msg.tracks || [];
				broadcast(drivers, { type: "cf-session", sessionId: phoneSessionId, tracks: phoneTracks });
			}
		});

		ws.on("close", () => {
			phone = null;
			phoneSessionId = null;
			phoneTracks = [];
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
					advanceLockQueue();
					if (lockHolder === null) { notifyAllDrivers(); notifyOperator(); }
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
	const ip = (req.headers['x-forwarded-for'] || req.socket.remoteAddress || '').split(',')[0].trim();
	const driverName = genName();
	drivers.set(ws, { name: driverName, ip, geo: null, ping: null, pingsSent: 0, pingSentAt: 0 });
	console.log(`Driver connected: ${driverName} (${ip}) (${driverCount()} total)`);

	// Geo lookup (async, update when ready)
	if (ip && ip !== '::1' && ip !== '127.0.0.1') {
		fetch(`https://ip.hackclub.com/ip/${ip}`)
			.then(r => r.json())
			.then(geo => {
				const d = drivers.get(ws);
				if (d) {
					d.geo = geo;
					notifyOperator();
				}
			})
			.catch(() => {});
	}

	// Initial ping immediately
	const driverInfo0 = drivers.get(ws);
	if (driverInfo0) driverInfo0.pingSentAt = Date.now();
	ws.send(JSON.stringify({ type: 'ping' }));

	// Ping every 5s
	const pingInterval = setInterval(() => {
		if (ws.readyState !== 1) { clearInterval(pingInterval); return; }
		const d = drivers.get(ws);
		if (d) { d.pingSentAt = Date.now(); d.pingsSent++; }
		ws.send(JSON.stringify({ type: 'ping' }));
	}, 5000);

	notifyOperator();
	notifyAllDrivers();

	// Send current phone state
	if (phone) ws.send(JSON.stringify({ type: "phone-joined" }));
	if (phoneSessionId) ws.send(JSON.stringify({ type: "cf-session", sessionId: phoneSessionId, tracks: phoneTracks }));

	ws.on("message", (data) => {
		const msg = JSON.parse(data.toString());

		// Torch — forward to phone
		if (msg.type === "torch") {
			if (phone && phone.readyState === 1) phone.send(data.toString());
			return;
		}

		// Pong — record RTT
		if (msg.type === "pong") {
			const d = drivers.get(ws);
			if (d && d.pingSentAt) {
				d.ping = Date.now() - d.pingSentAt;
				notifyOperator();
			}
			return;
		}

		// Request lock — join queue or get it immediately
		if (msg.type === "request-lock") {
			if (lockHolder === null && lockQueue.length === 0) {
				lockHolder = ws;
				console.log(`Lock granted to ${driverName}`);
				startLockTimer();
				notifyAllDrivers();
				notifyOperator();
			} else if (lockHolder !== ws && !lockQueue.includes(ws)) {
				lockQueue.push(ws);
				console.log(`${driverName} queued for lock (pos ${lockQueue.length})`);
				notifyAllDrivers();
				notifyOperator();
			}
			return;
		}

		// Cancel queue request
		if (msg.type === "cancel-lock-request") {
			const idx = lockQueue.indexOf(ws);
			if (idx !== -1) {
				lockQueue.splice(idx, 1);
				notifyAllDrivers();
				notifyOperator();
			}
			return;
		}

		// Release lock — advance queue
		if (msg.type === "release-lock") {
			if (lockHolder === ws) {
				lockHolder = null;
				stopLockTimer();
				console.log(`Lock released by ${driverName}`);
				advanceLockQueue();
				if (lockHolder === null) { notifyAllDrivers(); notifyOperator(); }
			}
			return;
		}

		// Control frame — only from lock holder, forward to operator
		if (msg.type === "frame") {
			if (ws === lockHolder) {
				lockLastFrameAt = Date.now();
				if (operator && operator.readyState === 1) operator.send(data.toString());
			}
			return;
		}
	});

	ws.on("close", () => {
		clearInterval(pingInterval);
		const name = drivers.get(ws)?.name;
		drivers.delete(ws);
		// Remove from queue
		const idx = lockQueue.indexOf(ws);
		if (idx !== -1) lockQueue.splice(idx, 1);
		if (lockHolder === ws) {
			lockHolder = null;
			console.log(`Lock holder ${name} disconnected — advancing queue`);
			advanceLockQueue();
		}
		if (streamDriver === ws) streamDriver = null;
		console.log(`Driver disconnected: ${name} (${driverCount()} remaining)`);
		if (lockHolder === null) { notifyAllDrivers(); notifyOperator(); }
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
