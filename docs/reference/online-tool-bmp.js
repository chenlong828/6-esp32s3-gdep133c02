
// 真实调色板 + 驱动纯色映射
const REAL_PALETTE = [[255,255,255],[0,0,0],[160,32,32],[240,224,80],[80,128,184],[96,128,80]];
const DRIVER_PALETTE = [[255,255,255],[0,0,0],[255,0,0],[255,255,0],[0,0,255],[0,255,0]];
const COLOR_MAP = new Map();
REAL_PALETTE.forEach((real, i) => COLOR_MAP.set(real.toString(), DRIVER_PALETTE[i]));

function closestColor(c, palette) {
  let best = palette[0], minD = Infinity;
  for (const p of palette) {
    const d = Math.pow(c[0]-p[0],2) + Math.pow(c[1]-p[1],2) + Math.pow(c[2]-p[2],2);
    if (d < minD) { minD = d; best = p; }
  }
  return best;
}

// 标准 Floyd-Steinberg
function floydDither(imgData) {
  const w = imgData.width, h = imgData.height, data = imgData.data;
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 4;
      const old = [data[i], data[i+1], data[i+2]];
      const realC = closestColor(old, REAL_PALETTE);
      data[i] = realC[0]; data[i+1] = realC[1]; data[i+2] = realC[2];
      const err = [old[0]-realC[0], old[1]-realC[1], old[2]-realC[2]];
      const diff = (dx, dy, f) => {
        const nx = x + dx, ny = y + dy;
        if (nx >= 0 && nx < w && ny >= 0 && ny < h) {
          const ni = (ny * w + nx) * 4;
          data[ni] += err[0] * f;
          data[ni+1] += err[1] * f;
          data[ni+2] += err[2] * f;
        }
      };
      diff(1,0,7/16); diff(-1,1,3/16); diff(0,1,5/16); diff(1,1,1/16);
    }
  }
}

// Floyd-Steinberg 蛇形扫描
function floydSerpentineDither(imgData) {
  const w = imgData.width, h = imgData.height, data = imgData.data;
  for (let y = 0; y < h; y++) {
    const leftToRight = (y % 2 === 0);
    const startX = leftToRight ? 0 : w - 1;
    const endX = leftToRight ? w : -1;
    const step = leftToRight ? 1 : -1;
    for (let x = startX; leftToRight ? x < endX : x > endX; x += step) {
      const i = (y * w + x) * 4;
      const old = [data[i], data[i+1], data[i+2]];
      const realC = closestColor(old, REAL_PALETTE);
      data[i] = realC[0]; data[i+1] = realC[1]; data[i+2] = realC[2];
      const err = [old[0]-realC[0], old[1]-realC[1], old[2]-realC[2]];
      const diff = (dx, dy, f) => {
        const nx = x + dx * step;
        const ny = y + dy;
        if (nx >= 0 && nx < w && ny >= 0 && ny < h) {
          const ni = (ny * w + nx) * 4;
          data[ni] += err[0] * f;
          data[ni+1] += err[1] * f;
          data[ni+2] += err[2] * f;
        }
      };
      diff(1,0,7/16); diff(-1,1,3/16); diff(0,1,5/16); diff(1,1,1/16);
    }
  }
}

// Atkinson
function atkinsonDither(imgData) {
  const w = imgData.width, h = imgData.height, data = imgData.data;
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const i = (y * w + x) * 4;
    const old = [data[i], data[i+1], data[i+2]];
    const realC = closestColor(old, REAL_PALETTE);
    data[i] = realC[0]; data[i+1] = realC[1]; data[i+2] = realC[2];
    const err = [(old[0]-realC[0])/8, (old[1]-realC[1])/8, (old[2]-realC[2])/8];
    const diff = (dx, dy, f) => {
      const nx = x + dx, ny = y + dy;
      if (nx >= 0 && nx < w && ny >= 0 && ny < h) {
        const ni = (ny * w + nx) * 4;
        data[ni] += err[0] * f; data[ni+1] += err[1] * f; data[ni+2] += err[2] * f;
      }
    };
    diff(1,0,1); diff(2,0,1); diff(-1,1,1); diff(0,1,1); diff(1,1,1); diff(0,2,1);
  }
}

// Stucki
function stuckiDither(imgData) {
  const w = imgData.width, h = imgData.height, data = imgData.data;
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const i = (y * w + x) * 4;
    const old = [data[i], data[i+1], data[i+2]];
    const realC = closestColor(old, REAL_PALETTE);
    data[i] = realC[0]; data[i+1] = realC[1]; data[i+2] = realC[2];
    const err = [old[0]-realC[0], old[1]-realC[1], old[2]-realC[2]];
    const total = 42;
    const diff = (dx, dy, weight) => {
      const nx = x + dx, ny = y + dy;
      if (nx >= 0 && nx < w && ny >= 0 && ny < h) {
        const ni = (ny * w + nx) * 4;
        const f = weight / total;
        data[ni] += err[0] * f; data[ni+1] += err[1] * f; data[ni+2] += err[2] * f;
      }
    };
    diff(1,0,8); diff(2,0,4);
    diff(-2,1,2); diff(-1,1,4); diff(0,1,8); diff(1,1,4); diff(2,1,2);
    diff(-2,2,1); diff(-1,2,2); diff(0,2,4); diff(1,2,2); diff(2,2,1);
  }
}

// Jarvis-Judice-Ninke
function jjnDither(imgData) {
  const w = imgData.width, h = imgData.height, data = imgData.data;
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const i = (y * w + x) * 4;
    const old = [data[i], data[i+1], data[i+2]];
    const realC = closestColor(old, REAL_PALETTE);
    data[i] = realC[0]; data[i+1] = realC[1]; data[i+2] = realC[2];
    const err = [old[0]-realC[0], old[1]-realC[1], old[2]-realC[2]];
    const total = 48;
    const diff = (dx, dy, weight) => {
      const nx = x + dx, ny = y + dy;
      if (nx >= 0 && nx < w && ny >= 0 && ny < h) {
        const ni = (ny * w + nx) * 4;
        const f = weight / total;
        data[ni] += err[0] * f; data[ni+1] += err[1] * f; data[ni+2] += err[2] * f;
      }
    };
    diff(1,0,7); diff(2,0,5);
    diff(-2,1,3); diff(-1,1,5); diff(0,1,7); diff(1,1,5); diff(2,1,3);
    diff(-2,2,1); diff(-1,2,3); diff(0,2,5); diff(1,2,3); diff(2,2,1);
  }
}

// 替换为驱动纯色
function remapToDriver(imgData) {
  const data = imgData.data;
  for (let i = 0; i < data.length; i += 4) {
    const key = [data[i], data[i+1], data[i+2]].toString();
    const mapped = COLOR_MAP.get(key);
    if (mapped) {
      data[i] = mapped[0]; data[i+1] = mapped[1]; data[i+2] = mapped[2];
    }
  }
}

// 处理多张图片
let processedImages = []; // 存储处理后的 canvas 和文件名
let processedHexData = []; // 存储处理后的 HEX 数据 + 原始字节

function processImages() {
  const files = document.getElementById('upload').files;
  if (files.length === 0) return alert('Please upload at least one image.');

  const [targetWidth, targetHeight] = document.getElementById('resolution').value.split(',').map(Number);
  const type = document.getElementById('ditherType').value;

  const originalPreview = document.getElementById('originalPreview');
  const resultPreview = document.getElementById('resultPreview');
  originalPreview.innerHTML = '';
  resultPreview.innerHTML = '';
  processedImages = [];
  processedHexData = [];

  document.getElementById('downloadBtn').style.display = 'none';
  document.getElementById('downloadBinBtn').style.display = 'none';

  Array.from(files).forEach((file, index) => {
    const img = new Image();
    img.onload = () => {
      // 原图预览
      const origCanvas = document.createElement('canvas');
      origCanvas.width = img.width;
      origCanvas.height = img.height;
      origCanvas.getContext('2d').drawImage(img, 0, 0);
      const origDiv = document.createElement('div');
      origDiv.className = 'preview-item';
      const origTitle = document.createElement('h4');
      origTitle.textContent = `Original ${index + 1}`;
      origDiv.appendChild(origTitle);
      origDiv.appendChild(origCanvas);
      originalPreview.appendChild(origDiv);

      // 处理结果画布：直接铺满目标分辨率（拉伸变形）
      const resCanvas = document.createElement('canvas');
      resCanvas.width = targetWidth;
      resCanvas.height = targetHeight;
      const ctx = resCanvas.getContext('2d');

      // 铺满画布：直接拉伸到目标尺寸（不保持比例）
      ctx.drawImage(img, 0, 0, targetWidth, targetHeight);

      const imgData = ctx.getImageData(0, 0, targetWidth, targetHeight);

      // 应用抖动
      if (type === 'floydSerpentine') floydSerpentineDither(imgData);
      else if (type === 'atkinson') atkinsonDither(imgData);
      else if (type === 'stucki') stuckiDither(imgData);
      else if (type === 'jjn') jjnDither(imgData);
      else floydDither(imgData);

      // 映射到驱动颜色
      remapToDriver(imgData);

      ctx.putImageData(imgData, 0, 0);

      // 结果预览
      const resDiv = document.createElement('div');
      resDiv.className = 'preview-item';
      const resTitle = document.createElement('h4');
      resTitle.textContent = `Output ${index + 1} (Fill ${targetWidth}×${targetHeight})`;
      resDiv.appendChild(resTitle);
      resDiv.appendChild(resCanvas);
      resultPreview.appendChild(resDiv);

      // 保存 canvas 用于下载
      processedImages.push({ canvas: resCanvas, name: `image${index}.bmp` });

      // 生成 HEX 数据（six color packed）
      const bytesData = imageDataToSixColorBytes(imgData);
      const hexData = bytesToHexLines(bytesData);
      processedHexData.push({ hex: hexData, bytes: bytesData, name: `image${index}.hex` });

      // 全部处理完后显示下载按钮
      if (index === files.length - 1) {
        document.getElementById('downloadBtn').style.display = 'inline-block';
        document.getElementById('downloadBinBtn').style.display = 'inline-block';
      }
    };
    img.src = URL.createObjectURL(file);
  });
}

// 下载所有 BMP
function downloadResults() {
  processedImages.forEach((item, index) => {
    const a = document.createElement('a');
    a.download = item.name;
    const bmpBlob = canvasToBmpBlob(item.canvas);
    const url = URL.createObjectURL(bmpBlob);
    a.href = url;
    a.click();
    URL.revokeObjectURL(url);
  });
}

// 下载 HEX（包含帧间隔元数据）
function downloadBinResults() {
  if (!processedHexData.length) return;

  const interval = parseInt(document.getElementById('frame-interval').value || '0', 10) || 0;
  const resolution = document.getElementById('resolution').value.split(',').map(Number);
  const width = resolution[0];
  const height = resolution[1];
  const frames = processedHexData.length;

  // Header: magic[8]="EPD6BIN\0", uint16 width, uint16 height, uint16 frames, uint32 intervalMs (LE)
  const header = new ArrayBuffer(8 + 2 + 2 + 2 + 4);
  const dv = new DataView(header);
  const magic = new TextEncoder().encode('EPD6BIN');
  for (let i = 0; i < 8; i++) {
    dv.setUint8(i, magic[i] || 0);
  }
  dv.setUint16(8, width, true);
  dv.setUint16(10, height, true);
  dv.setUint16(12, frames, true);
  dv.setUint32(14, interval, true);

  const parts = [header];
  processedHexData.forEach(item => parts.push(item.bytes));

  const blob = new Blob(parts, { type: 'application/octet-stream' });
  const a = document.createElement('a');
  a.download = 'images.bin';
  a.href = URL.createObjectURL(blob);
  a.click();
  URL.revokeObjectURL(a.href);
}

// 将 canvas 转换为真实 BMP Blob（24-bit，无压缩）
function canvasToBmpBlob(canvas) {
  const w = canvas.width;
  const h = canvas.height;
  const ctx = canvas.getContext('2d');
  const imageData = ctx.getImageData(0, 0, w, h).data;

  const rowSize = (w * 3 + 3) & ~3; // 每行按 4 字节对齐
  const pixelArraySize = rowSize * h;
  const fileSize = 54 + pixelArraySize;

  const buffer = new ArrayBuffer(fileSize);
  const dv = new DataView(buffer);

  // BITMAPFILEHEADER (14 bytes)
  dv.setUint8(0, 0x42); // 'B'
  dv.setUint8(1, 0x4D); // 'M'
  dv.setUint32(2, fileSize, true);
  dv.setUint32(6, 0, true);
  dv.setUint32(10, 54, true);

  // BITMAPINFOHEADER (40 bytes)
  dv.setUint32(14, 40, true);
  dv.setInt32(18, w, true);
  dv.setInt32(22, h, true);
  dv.setUint16(26, 1, true);
  dv.setUint16(28, 24, true);
  dv.setUint32(30, 0, true);
  dv.setUint32(34, pixelArraySize, true);
  dv.setInt32(38, 2835, true); // 72 DPI
  dv.setInt32(42, 2835, true);
  dv.setUint32(46, 0, true);
  dv.setUint32(50, 0, true);

  // 像素数据（BGR，底部行优先）
  for (let y = 0; y < h; y++) {
    const srcRow = (h - 1 - y) * w * 4;
    const dstRow = 54 + y * rowSize;
    for (let x = 0; x < w; x++) {
      const i = srcRow + x * 4;
      const j = dstRow + x * 3;
      dv.setUint8(j, imageData[i + 2]);     // B
      dv.setUint8(j + 1, imageData[i + 1]); // G
      dv.setUint8(j + 2, imageData[i]);     // R
    }
  }

  return new Blob([buffer], { type: 'image/bmp' });
}

// 将 ImageData 转换为 6 色模式的 HEX 字符串（每字节2像素）
function imageDataToSixColorBytes(imageData) {
  const width = imageData.width;
  const height = imageData.height;
  const data = imageData.data;
  const totalBytes = Math.ceil((width * height) / 2);
  const processedData = new Uint8Array(totalBytes);

  function rgbToSixColor(r, g, b) {
    if (r < 128 && g < 128 && b < 128) return 0x00;  // Black
    if (r > 128 && g > 128 && b > 128) return 0x01;  // White
    if (r > 128 && g < 128 && b < 128) return 0x03;  // Red
    if (r > 128 && g > 128 && b < 128) return 0x02;  // Yellow
    if (r < 128 && g > 128 && b < 128) return 0x06;  // Green
    if (r < 128 && g < 128 && b > 128) return 0x05;  // Blue
    return 0x01;
  }

  let outIndex = 0;
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x += 2) {
      const index1 = (y * width + x) * 4;
      const index2 = (y * width + x + 1) * 4;

      const r1 = data[index1];
      const g1 = data[index1 + 1];
      const b1 = data[index1 + 2];

      let r2 = 255, g2 = 255, b2 = 255;
      if (x + 1 < width) {
        r2 = data[index2];
        g2 = data[index2 + 1];
        b2 = data[index2 + 2];
      }

      const colorValue1 = rgbToSixColor(r1, g1, b1);
      const colorValue2 = rgbToSixColor(r2, g2, b2);
      processedData[outIndex++] = (colorValue1 << 4) | colorValue2;
    }
  }

  return processedData;
}

// Uint8Array -> HEX 字符串（每行 16 字节）
function bytesToHexLines(bytes) {
  const hexLines = [];
  for (let i = 0; i < bytes.length; i += 16) {
    const slice = bytes.slice(i, i + 16);
    const line = Array.from(slice).map(b => b.toString(16).padStart(2, '0')).join(' ');
    hexLines.push(line);
  }
  return hexLines.join('\n');
}
