// Native OpenSSL ChaCha20 acceleration for local resource analysis only.
const crypto = require('node:crypto');
const chunks = [];
process.stdin.on('data', chunk => chunks.push(chunk));
process.stdin.on('end', () => {
  const input = Buffer.concat(chunks);
  const key = input.subarray(0, 32);
  if (process.argv[2] === 'index') {
    const records = input.subarray(32);
    if (records.length % 28) throw new Error('Invalid index block size');
    const output = Buffer.alloc(records.length);
    for (let i = 0; i < records.length / 28; ++i) {
      const iv = Buffer.alloc(16);
      iv.write('idx', 4, 'ascii'); iv.writeUInt32LE(i, 8);
      const cipher = crypto.createCipheriv('chacha20', key, iv);
      cipher.update(records.subarray(i * 28, i * 28 + 28)).copy(output, i * 28);
      cipher.final();
    }
    process.stdout.write(output);
  } else {
    if (input.length < 52) throw new Error('Truncated cipher input');
    const offset = Number(input.readBigUInt64LE(44));
    const iv = Buffer.alloc(16);
    iv.writeUInt32LE(Math.floor(offset / 64), 0); input.copy(iv, 4, 32, 44);
    const cipher = crypto.createCipheriv('chacha20', key, iv);
    if (offset % 64) cipher.update(Buffer.alloc(offset % 64));
    process.stdout.write(cipher.update(input.subarray(52)));
    process.stdout.write(cipher.final());
  }
});
