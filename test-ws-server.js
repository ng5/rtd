// Simple WebSocket Test Server for RTD Excel
// Sends random price updates for BTC, EURUSD, GOLD, and AAPL

const WebSocket = require('ws');

const PORT = 8080;
const wss = new WebSocket.Server({ port: PORT });

console.log(`WebSocket server started on ws://localhost:${PORT}`);
console.log('Sending random price updates every 1 second...\n');

// Track connected clients
let clients = new Set();

wss.on('connection', (ws) => {
  console.log('New client connected');
  clients.add(ws);

  ws.on('message', (message) => {
    console.log('Received:', message.toString());
  });

  ws.on('close', () => {
    console.log('Client disconnected');
    clients.delete(ws);
  });

  // Send welcome message
  ws.send(JSON.stringify({
    message: 'Connected to RTD test server',
    topics: ['BTC', 'EURUSD', 'GOLD', 'AAPL']
  }));
});

// Broadcast price updates every 1 second
setInterval(() => {
  if (clients.size === 0) return;

  const topics = [
    { topic: 'BTC', base: 45000, range: 1000 },
    { topic: 'EURUSD', base: 1.09, range: 0.01 },
    { topic: 'GOLD', base: 2050, range: 20 },
    { topic: 'AAPL', base: 180, range: 5 }
  ];

  topics.forEach(({ topic, base, range }) => {
    const value = base + (Math.random() - 0.5) * range;
    const message = JSON.stringify({
      topic: topic,
      value: parseFloat(value.toFixed(4))
    });

    clients.forEach(client => {
      if (client.readyState === WebSocket.OPEN) {
        client.send(message);
      }
    });

    console.log(`Sent: ${message}`);
  });

  console.log('---');
}, 1000);

// Handle server errors
wss.on('error', (error) => {
  console.error('Server error:', error);
});

console.log('\nTest in Excel with these formulas:');
console.log('=RTD("mycompany.rtdtickcpp",, "ws://localhost:8080", "BTC")');
console.log('=RTD("mycompany.rtdtickcpp",, "ws://localhost:8080", "EURUSD")');
console.log('=RTD("mycompany.rtdtickcpp",, "ws://localhost:8080", "GOLD")');
console.log('=RTD("mycompany.rtdtickcpp",, "ws://localhost:8080", "AAPL")');
console.log('\nPress Ctrl+C to stop the server\n');
