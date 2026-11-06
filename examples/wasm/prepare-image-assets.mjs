import { copyFile, readFile, writeFile } from "node:fs/promises";
import { join } from "node:path";

const [sourceRoot, distRoot] = process.argv.slice(2);

if (!sourceRoot || !distRoot) {
  throw new Error("用法: node prepare-image-assets.mjs <source-dir> <dist-dir>");
}

const sourceManifest = join(sourceRoot, "image-demo-vectors.json");
const demo = JSON.parse(await readFile(sourceManifest, "utf8"));
await Promise.all(demo.images.map(async item => {
  await copyFile(join(sourceRoot, item.resource), join(distRoot, item.resource));
}));

await writeFile(join(distRoot, "image-demo-vectors.json"), JSON.stringify(demo));
