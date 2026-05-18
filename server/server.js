const express = require('express');
const http = require('http');
const { WebSocketServer } = require('ws');
const path = require('path');

const app = express();
const server = http.createServer(app);
const wss = new WebSocketServer({ server });

app.use(express.static(path.join(__dirname, 'public')));

app.get('/controller', (req, res) => {
  res.sendFile(path.join(__dirname, '..', 'esp32-firmware', 'controller.html'));
});

let phone = null;
let controller = null;

wss.on('connection', (ws, req) => {
  const role = new URL(req.url, 'http://localhost').searchParams.get('role');

  if (role === 'phone') {
    phone = ws;
    console.log('Phone connected');
    if (controller) controller.send(JSON.stringify({ type: 'phone-joined' }));
  } else {
    controller = ws;
    console.log('Controller connected');
    if (phone) controller.send(JSON.stringify({ type: 'phone-joined' }));
  }

  ws.on('message', (data) => {
    const peer = ws === phone ? controller : phone;
    if (peer && peer.readyState === 1) {
      peer.send(data.toString());
    }
  });

  ws.on('close', () => {
    if (ws === phone) {
      phone = null;
      console.log('Phone disconnected');
      if (controller) controller.send(JSON.stringify({ type: 'phone-left' }));
    } else {
      controller = null;
      console.log('Controller disconnected');
    }
  });
});

const PORT = process.env.PORT || 3000;
server.listen(PORT, '0.0.0.0', () => {
  const nets = require('os').networkInterfaces();
  let localIP = 'localhost';
  for (const name of Object.keys(nets)) {
    for (const net of nets[name]) {
      if (net.family === 'IPv4' && !net.internal) {
        localIP = net.address;
        break;
      }
    }
  }
  console.log(`\nServer running on port ${PORT}`);
  console.log(`  Local:     http://localhost:${PORT}/controller`);
  console.log(`  Network:   http://${localIP}:${PORT}/controller`);
  console.log(`\nUse ngrok for phone access:`);
  console.log(`  ngrok http ${PORT}\n`);
});
