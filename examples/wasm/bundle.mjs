import { readFile, writeFile } from "node:fs/promises";
import { extname } from "node:path";
import { gzipSync } from "node:zlib";

const demoRoot = new URL("./", import.meta.url);
const distRoot = new URL("./dist/", demoRoot);

const [html, css, appSource, runtimeSource, wasm, demo, document, imageDemo] = await Promise.all([
  readFile(new URL("index.html", demoRoot), "utf8"),
  readFile(new URL("styles.css", demoRoot), "utf8"),
  readFile(new URL("app.js", demoRoot), "utf8"),
  readFile(new URL("vexdb-wasm.js", distRoot), "utf8"),
  readFile(new URL("vexdb-wasm.wasm", distRoot)),
  readFile(new URL("embedding-demo-vectors.json", distRoot)),
  readFile(new URL("embedding-demo.txt", distRoot)),
  readFile(new URL("image-demo-vectors.json", distRoot))
]);

const imageResources = JSON.parse(imageDemo.toString("utf8")).images.map(item => item.resource);
const imageMimeTypes = {
  ".avif": "image/avif",
  ".jpeg": "image/jpeg",
  ".jpg": "image/jpeg",
  ".png": "image/png",
  ".webp": "image/webp"
};
const embeddedImages = Object.fromEntries(await Promise.all(imageResources.map(async resource => {
  const bytes = await readFile(new URL(resource, distRoot));
  const mimeType = imageMimeTypes[extname(resource).toLowerCase()];
  if (!mimeType) throw new Error(`不支持嵌入该图片格式：${resource}`);
  return [resource, `data:${mimeType};base64,${bytes.toString("base64")}`];
})));

const runtimeWithoutExport = runtimeSource.replace(/export default createVexDBWasm;\s*$/, "");
const appWithoutImport = appSource.replace(
  /^import createVexDBWasm from "\.\/dist\/vexdb-wasm\.js";\s*/,
  ""
);
if (runtimeWithoutExport === runtimeSource || appWithoutImport === appSource) {
  throw new Error("Emscripten 或页面入口格式已经变化，无法安全生成单文件 HTML");
}

const gzipBase64 = bytes => gzipSync(bytes, { level: 9 }).toString("base64");
const assets = {
  compression: "gzip",
  wasm: gzipBase64(wasm),
  demo: gzipBase64(demo),
  document: gzipBase64(document),
  imageDemo: gzipBase64(imageDemo),
  images: embeddedImages
};
const inlineScript = `globalThis.__VEXDB_STANDALONE__ = ${JSON.stringify(assets)};\n${runtimeWithoutExport}\n${appWithoutImport}`;
const standalone = html
  .replace('<link rel="stylesheet" href="./styles.css">', `<style>\n${css}\n</style>`)
  .replace('<script type="module" src="./app.js"></script>', `<script type="module">\n${inlineScript}\n</script>`);

if (standalone === html || standalone.includes('src="./app.js"')) {
  throw new Error("单文件 HTML 模板替换失败");
}

const output = new URL("vexdb-lite-demo.html", distRoot);
await writeFile(output, standalone);
console.log(`单文件 HTML 已生成: ${output.pathname}`);
