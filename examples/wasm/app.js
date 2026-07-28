import createVexDBWasm from "./dist/vexdb-wasm.js";

let standaloneAssets = globalThis.__VEXDB_STANDALONE__ ?? null;

const DEFAULT_ENDPOINT = "https://dashscope.aliyuncs.com/compatible-mode/v1";
const DEFAULT_MODEL = "text-embedding-v4";
const DEFAULT_IMAGE_ENDPOINT = "https://dashscope.aliyuncs.com/api/v1/services/embeddings/multimodal-embedding/multimodal-embedding";
const DEFAULT_IMAGE_MODEL = "tongyi-embedding-vision-plus";
const MAX_IMAGES = 20;
const MAX_IMAGE_BYTES = 12 * 1024 * 1024;
const STORAGE_KEYS = {
  endpoint: "vexdb.demo.embedding.endpoint",
  model: "vexdb.demo.embedding.model",
  apiKey: "vexdb.demo.embedding.api-key",
  imageEndpoint: "vexdb.demo.image-embedding-v2.endpoint",
  imageModel: "vexdb.demo.image-embedding-v2.model",
  imageApiKey: "vexdb.demo.image-embedding-v2.api-key"
};

const elements = Object.fromEntries([
  "runtimeBadge", "textModeTab", "imageModeTab", "defaultTab", "customTab",
  "defaultTabIcon", "defaultTabTitle", "defaultTabCaption", "customTabIcon",
  "customTabTitle", "customTabCaption", "defaultPanel", "customPanel",
  "imageDefaultPanel", "imageCustomPanel", "defaultStatus", "chunkCount",
  "dimensions", "documentPreviewButton", "documentPreview", "showChunks",
  "presetQuestions", "searchForm", "queryInput", "searchButton", "queryHint",
  "defaultSettingsShortcut", "resultSection", "queryTime", "results",
  "planSection", "queryPlan", "customStatus", "customProgress", "settingsCard",
  "settingsSummary", "endpointInput", "modelInput", "apiKeyInput",
  "testEmbeddingButton", "saveSettingsButton", "settingsMessage",
  "toggleEditorButton", "sourceEditorWrap", "sourceText", "sourceCount",
  "fillSampleText", "sourceCollapsed", "sourceCollapsedText", "chunkLength",
  "overlapLength", "chunkPreviewSummary", "toggleChunkPreview",
  "customChunkPreview", "generateHint", "generateVectorsButton",
  "customSearchForm", "customQueryInput", "customSearchButton", "customRowCount",
  "customDimensions", "customResultSection", "customQueryTime", "customResults",
  "customPlanSection", "customQueryPlan", "imageDefaultStatus", "imageCount",
  "imageDimensions", "imageGallery", "imageQuestions", "imageSearchForm",
  "imageQueryInput", "imageSearchButton", "imageQueryHint",
  "imageDefaultSettingsShortcut", "imageResultSection", "imageQueryTime",
  "imageResults", "imagePlanSection", "imageQueryPlan", "customImageStatus",
  "customImageProgress", "imageSettingsCard", "imageSettingsSummary",
  "imageEndpointInput", "imageModelInput", "imageApiKeyInput",
  "testImageEmbeddingButton", "saveImageSettingsButton", "imageSettingsMessage",
  "selectedImageCount", "imageFileInput", "fillSampleImages", "customImageGallery",
  "customImageGenerateHint", "generateImageVectorsButton", "customImageSearchForm",
  "customImageQueryInput", "customImageSearchButton", "customImageRowCount",
  "customImageDimensions", "customImageResultSection", "customImageQueryTime",
  "customImageResults", "customImagePlanSection", "customImageQueryPlan",
  "versionText", "backToTop", "chunksDialog", "closeChunks", "dialogMeta",
  "chunkList"
].map(id => [id, document.querySelector(`#${id}`)]));

let demo = null;
let imageDemo = null;
let documentText = "";
let imageSources = {};
let runtimeModule = null;
let defaultApi = null;
let imageDefaultApi = null;
let customApi = null;
let customImageApi = null;
let customDimensions = 0;
let customImageDimensions = 0;
let customChunks = [];
let customImageItems = [];
let selectedQuery = null;
let selectedImageQuery = null;
let generatedSignature = "";
let customImageGeneratedSignature = "";
let customBusy = false;
let customImageBusy = false;
let currentView = { mode: "text", source: "default" };
let scrollFrame = 0;

function decodeBase64Bytes(value) {
  const binary = atob(value);
  const bytes = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; index += 1) bytes[index] = binary.charCodeAt(index);
  return bytes;
}

async function decompressEmbeddedBytes(value, compression) {
  const bytes = decodeBase64Bytes(value);
  if (!compression) return bytes;
  if (compression !== "gzip") throw new Error(`不支持的单文件压缩格式：${compression}`);
  if (typeof DecompressionStream !== "function") {
    throw new Error("当前浏览器版本过旧，无法打开压缩后的单文件演示");
  }
  const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("gzip"));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

async function loadPageAssets() {
  if (!standaloneAssets) {
    const [demoResponse, documentResponse, imageDemoResponse] = await Promise.all([
      fetch("./dist/embedding-demo-vectors.json"),
      fetch("./dist/embedding-demo.txt"),
      fetch("./dist/image-demo-vectors.json")
    ]);
    if (!demoResponse.ok || !documentResponse.ok || !imageDemoResponse.ok) {
      throw new Error("默认示例资源读取失败");
    }
    const loadedImageDemo = await imageDemoResponse.json();
    const loadedImageSources = Object.fromEntries(await Promise.all(loadedImageDemo.images.map(async item => {
      const response = await fetch(`./dist/${item.resource}`);
      if (!response.ok) throw new Error(`图片资源读取失败：${item.resource}`);
      const contentType = response.headers.get("Content-Type");
      const mimeType = contentType?.startsWith("image/") ? contentType : "image/png";
      const blob = new Blob([await response.arrayBuffer()], { type: mimeType });
      return [item.resource, await blobToDataUrl(blob)];
    })));
    return {
      demo: await demoResponse.json(),
      imageDemo: loadedImageDemo,
      documentText: (await documentResponse.text()).trim(),
      imageSources: loadedImageSources,
      createModule: () => createVexDBWasm({ locateFile: file => `./dist/${file}` })
    };
  }

  const embedded = standaloneAssets;
  const [wasmBytes, demoBytes, documentBytes, imageDemoBytes] = await Promise.all([
    decompressEmbeddedBytes(embedded.wasm, embedded.compression),
    decompressEmbeddedBytes(embedded.demo, embedded.compression),
    decompressEmbeddedBytes(embedded.document, embedded.compression),
    decompressEmbeddedBytes(embedded.imageDemo, embedded.compression)
  ]);
  const wasmModule = await WebAssembly.compile(wasmBytes);
  const loaded = {
    demo: JSON.parse(new TextDecoder().decode(demoBytes)),
    imageDemo: JSON.parse(new TextDecoder().decode(imageDemoBytes)),
    documentText: new TextDecoder().decode(documentBytes).trim(),
    imageSources: embedded.images,
    createModule: () => createVexDBWasm({
      instantiateWasm(imports, receiveInstance) {
        const instance = new WebAssembly.Instance(wasmModule, imports);
        receiveInstance(instance);
        return instance.exports;
      }
    })
  };
  delete globalThis.__VEXDB_STANDALONE__;
  standaloneAssets = null;
  return loaded;
}

function createApi(module, scope) {
  const resetScope = module.cwrap("vexdb_wasm_reset_scope", "number", ["string", "number", "string"]);
  const insertScope = module.cwrap("vexdb_wasm_insert_scope", "number", ["string", "number", "string", "string", "string"]);
  const searchScope = module.cwrap("vexdb_wasm_search_scope", "string", ["string", "string", "number"]);
  const countScope = module.cwrap("vexdb_wasm_count_scope", "number", ["string"]);
  return {
    reset: dimensions => resetScope(scope, dimensions, "cosine"),
    resetWithMetric: (dimensions, metric) => resetScope(scope, dimensions, metric),
    insert: (rowid, embedding, title, content) => insertScope(scope, rowid, embedding, title, content),
    search: (query, k) => searchScope(scope, query, k),
    count: () => countScope(scope),
    version: module.cwrap("vexdb_wasm_version", "string", []),
    lastError: module.cwrap("vexdb_wasm_last_error", "string", [])
  };
}

function setRuntimeStatus(text, state = "loading") {
  elements.runtimeBadge.textContent = text;
  elements.runtimeBadge.dataset.state = state;
}

function setCustomStatus(text) { elements.customStatus.textContent = text; }
function setCustomImageStatus(text) { elements.customImageStatus.textContent = text; }

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#039;");
}

function imageTitle(resource) {
  const metadata = imageDemo?.images?.find(item => item.resource === resource);
  if (metadata?.caption) return metadata.caption;
  const name = resource.replace(/\.[^.]+$/, "");
  if (name === "image-demo-1") return "羊与汽车 · 版本 1";
  if (name.includes("sheep-car-")) return `羊与汽车 · 版本 ${name.match(/-(\d+)$/)?.[1] ?? "?"}`;
  if (name === "image-demo-2") return "LLM 输入输出流程 · 版本 1";
  if (name.includes("llm-")) return `LLM 输入输出流程 · 版本 ${name.match(/-(\d+)$/)?.[1] ?? "?"}`;
  if (name === "image-demo-3-real") return "智能体的记忆、规划和工具 · 版本 1";
  if (name.includes("agent-")) return `智能体的记忆、规划和工具 · 版本 ${name.match(/-(\d+)$/)?.[1] ?? "?"}`;
  return resource;
}

function safeRead(storageName, key, fallback = "") {
  try { return window[storageName].getItem(key) ?? fallback; } catch { return fallback; }
}

function safeWrite(storageName, key, value) {
  try {
    const storage = window[storageName];
    if (value) storage.setItem(key, value);
    else storage.removeItem(key);
  } catch {
    // file:// 的隐私模式可能禁用 Storage；页面仍可在当前内存中使用。
  }
}

function blobToDataUrl(blob) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(String(reader.result));
    reader.onerror = () => reject(new Error("图片资源转换失败"));
    reader.readAsDataURL(blob);
  });
}

function validatedUrl(endpoint, suffix, label) {
  let url;
  try { url = new URL(endpoint); } catch { throw new Error(`${label}地址格式不正确`); }
  if (!["http:", "https:"].includes(url.protocol)) throw new Error(`${label}地址只支持 HTTP 或 HTTPS`);
  const localHosts = new Set(["localhost", "127.0.0.1", "[::1]", "::1"]);
  if (url.protocol === "http:" && !localHosts.has(url.hostname)) throw new Error(`远程${label}地址必须使用 HTTPS`);
  url.pathname = url.pathname.replace(/\/+$/, "");
  if (!url.pathname.endsWith(suffix)) url.pathname += suffix;
  return url;
}

function currentSettings() {
  return {
    endpoint: elements.endpointInput.value.trim(),
    model: elements.modelInput.value.trim(),
    apiKey: elements.apiKeyInput.value.trim()
  };
}

function currentImageSettings() {
  return {
    endpoint: elements.imageEndpointInput.value.trim(),
    model: elements.imageModelInput.value.trim(),
    apiKey: elements.imageApiKeyInput.value.trim()
  };
}

function validateSettings() {
  const settings = currentSettings();
  const url = validatedUrl(settings.endpoint, "/embeddings", "API ");
  if (!settings.model) throw new Error("请填写模型名称");
  const needsKey = url.hostname === "api.openai.com" || url.hostname.endsWith("dashscope.aliyuncs.com");
  if (needsKey && !settings.apiKey) throw new Error("请填写 API Key");
  return { ...settings, url };
}

function validateImageSettings() {
  const settings = currentImageSettings();
  const url = validatedUrl(settings.endpoint, "/multimodal-embedding", "图片 API ");
  if (!settings.model) throw new Error("请填写图片模型名称");
  if (url.hostname.endsWith("dashscope.aliyuncs.com") && !settings.apiKey) throw new Error("请填写 API Key");
  return { ...settings, url };
}

function updateSettingsSummary() {
  try {
    const settings = currentSettings();
    const host = new URL(settings.endpoint).host;
    elements.settingsSummary.textContent = settings.model && host ? `${settings.model} · ${host}` : "填写 API 地址、模型和 API Key";
  } catch { elements.settingsSummary.textContent = "API 地址需要检查"; }
}

function updateImageSettingsSummary() {
  try {
    const settings = currentImageSettings();
    const host = new URL(settings.endpoint).host;
    elements.imageSettingsSummary.textContent = settings.model && host ? `${settings.model} · ${host}` : "填写多模态 API 地址、模型和 API Key";
  } catch { elements.imageSettingsSummary.textContent = "图片 API 地址需要检查"; }
}

function showSettingsMessage(message, state = "") {
  elements.settingsMessage.textContent = message;
  elements.settingsMessage.dataset.state = state;
}

function showImageSettingsMessage(message, state = "") {
  elements.imageSettingsMessage.textContent = message;
  elements.imageSettingsMessage.dataset.state = state;
}

function saveSettings() {
  try {
    const settings = validateSettings();
    safeWrite("localStorage", STORAGE_KEYS.endpoint, settings.endpoint);
    safeWrite("localStorage", STORAGE_KEYS.model, settings.model);
    safeWrite("sessionStorage", STORAGE_KEYS.apiKey, settings.apiKey);
    updateSettingsSummary();
    showSettingsMessage("设置已保存。API Key 只保留在当前浏览器会话。", "success");
    return settings;
  } catch (error) {
    showSettingsMessage(error instanceof Error ? error.message : String(error), "error");
    throw error;
  }
}

function saveImageSettings() {
  try {
    const settings = validateImageSettings();
    safeWrite("localStorage", STORAGE_KEYS.imageEndpoint, settings.endpoint);
    safeWrite("localStorage", STORAGE_KEYS.imageModel, settings.model);
    safeWrite("sessionStorage", STORAGE_KEYS.imageApiKey, settings.apiKey);
    updateImageSettingsSummary();
    showImageSettingsMessage("设置已保存。API Key 只保留在当前浏览器会话。", "success");
    return settings;
  } catch (error) {
    showImageSettingsMessage(error instanceof Error ? error.message : String(error), "error");
    throw error;
  }
}

async function requestJson(settings, body, label) {
  let response;
  try {
    response = await fetch(settings.url, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        ...(settings.apiKey ? { Authorization: `Bearer ${settings.apiKey}` } : {})
      },
      body: JSON.stringify(body)
    });
  } catch (error) {
    const detail = error instanceof Error && error.message ? `（${error.message}）` : "";
    throw new Error(`无法连接${label}。请确认地址可访问，并允许浏览器跨域请求。${detail}`);
  }
  let payload;
  try { payload = await response.json(); } catch { throw new Error(`${label}返回了无法识别的内容（HTTP ${response.status}）`); }
  if (!response.ok) {
    const detail = payload?.error?.message || payload?.message || `HTTP ${response.status}`;
    throw new Error(`${label}请求失败：${detail}`);
  }
  return payload;
}

function validateVectors(vectors, expectedCount, label) {
  if (!Array.isArray(vectors) || vectors.length !== expectedCount) throw new Error(`${label}返回的向量数量不正确`);
  const dimensions = vectors[0]?.length ?? 0;
  if (dimensions <= 0 || dimensions > 65535) throw new Error(`${label}返回的向量维度不正确`);
  for (const vector of vectors) {
    if (!Array.isArray(vector) || vector.length !== dimensions || vector.some(value => !Number.isFinite(value))) {
      throw new Error(`${label}返回了不完整的向量`);
    }
  }
  return vectors;
}

async function embedBatch(inputs) {
  if (!Array.isArray(inputs) || inputs.length === 0 || inputs.length > 64) throw new Error("每次只能生成 1 到 64 段文本的向量");
  const settings = validateSettings();
  const payload = await requestJson(settings, { model: settings.model, input: inputs }, "Embedding API");
  if (!Array.isArray(payload.data)) throw new Error("Embedding API 返回的格式不正确");
  const vectors = [...payload.data]
    .sort((left, right) => Number(left.index ?? 0) - Number(right.index ?? 0))
    .map(item => item.embedding);
  return validateVectors(vectors, inputs.length, "Embedding API");
}

async function embedImageContents(contents) {
  if (!Array.isArray(contents) || contents.length === 0 || contents.length > 8) throw new Error("每次只能生成 1 到 8 个图片或问题向量");
  const settings = validateImageSettings();
  const payload = await requestJson(settings, { model: settings.model, input: { contents } }, "图片 Embedding API");
  const entries = payload?.output?.embeddings;
  if (!Array.isArray(entries)) throw new Error("图片 Embedding API 返回的格式不正确");
  const vectors = [...entries]
    .sort((left, right) => Number(left.index ?? 0) - Number(right.index ?? 0))
    .map(item => item.embedding);
  return validateVectors(vectors, contents.length, "图片 Embedding API");
}

function activateView(mode, source) {
  currentView = { mode, source };
  elements.textModeTab.setAttribute("aria-selected", String(mode === "text"));
  elements.imageModeTab.setAttribute("aria-selected", String(mode === "image"));
  elements.textModeTab.tabIndex = mode === "text" ? 0 : -1;
  elements.imageModeTab.tabIndex = mode === "image" ? 0 : -1;
  elements.defaultTab.setAttribute("aria-selected", String(source === "default"));
  elements.customTab.setAttribute("aria-selected", String(source === "custom"));
  elements.defaultTab.tabIndex = source === "default" ? 0 : -1;
  elements.customTab.tabIndex = source === "custom" ? 0 : -1;

  const textMode = mode === "text";
  elements.defaultTabIcon.textContent = textMode ? "⌕" : "▧";
  elements.defaultTabTitle.textContent = textMode ? "默认文本示例" : "默认图片示例";
  elements.defaultTabCaption.textContent = textMode ? "内置向量，直接搜索" : "内置图片和问题向量";
  elements.customTabIcon.textContent = textMode ? "✎" : "+";
  elements.customTabTitle.textContent = textMode ? "自定义文本" : "自定义图片";
  elements.customTabCaption.textContent = textMode ? "输入、分片并生成向量" : "导入图片并生成向量";
  const panelId = {
    "text-default": "defaultPanel",
    "text-custom": "customPanel",
    "image-default": "imageDefaultPanel",
    "image-custom": "imageCustomPanel"
  }[`${mode}-${source}`];
  elements.defaultTab.setAttribute("aria-controls", mode === "text" ? "defaultPanel" : "imageDefaultPanel");
  elements.customTab.setAttribute("aria-controls", mode === "text" ? "customPanel" : "imageCustomPanel");
  [elements.defaultPanel, elements.customPanel, elements.imageDefaultPanel, elements.imageCustomPanel]
    .forEach(panel => { panel.hidden = panel.id !== panelId; });
  window.scrollTo({ top: 0, behavior: "smooth" });
}

function updateBackToTop() {
  const threshold = Math.max(520, window.innerHeight * 0.75);
  elements.backToTop.hidden = window.scrollY <= threshold;
}

function renderQuestions() {
  elements.presetQuestions.innerHTML = demo.queries.map((query, index) => `
    <button class="question-button" type="button" data-query-index="${index}" aria-pressed="false">
      <span class="question-mark">${index + 1}</span><span>${escapeHtml(query.text)}</span><span class="arrow" aria-hidden="true">→</span>
    </button>`).join("");
}

function renderImageQuestions() {
  elements.imageQuestions.innerHTML = `<div class="image-featured-questions">${imageDemo.queries.map((query, index) => `
    <button class="question-button" type="button" data-image-query-index="${index}" aria-pressed="false">
      <span class="question-mark">${index + 1}</span><span>${escapeHtml(query.text)}</span><span class="arrow" aria-hidden="true">→</span>
    </button>`).join("")}</div>`;
}

function renderDefaultChunks() {
  elements.dialogMeta.textContent = `${demo.chunks.length} 个分片 · ${demo.dimensions.toLocaleString("zh-CN")} 维向量 · 点击任意分片展开全文`;
  elements.chunkList.innerHTML = demo.chunks.map((chunk, index) => `
    <details class="chunk-item" ${index === 0 ? "open" : ""}>
      <summary><span class="chunk-title">文本片段 ${chunk.id}</span><span class="chunk-size">${chunk.text.length} 字</span><span class="chevron" aria-hidden="true">⌄</span></summary>
      <p class="chunk-content">${escapeHtml(chunk.text)}</p>
    </details>`).join("");
}

function renderDefaultImageGallery() {
  elements.imageGallery.innerHTML = imageDemo.images.map(item => `
    <figure class="gallery-image-card">
      <div class="image-frame"><img src="${escapeHtml(imageSources[item.resource])}" alt="${escapeHtml(imageTitle(item.resource))}"></div>
    </figure>`).join("");
}

function selectQuery(index) {
  selectedQuery = demo.queries[index];
  elements.queryInput.value = selectedQuery.text;
  elements.searchButton.disabled = !defaultApi;
  elements.defaultSettingsShortcut.hidden = true;
  elements.queryHint.textContent = "问题已填入。点击右侧发送按钮后，才会执行向量搜索。";
  elements.resultSection.hidden = true;
  elements.planSection.hidden = true;
  document.querySelectorAll("[data-query-index]").forEach((button, buttonIndex) => button.setAttribute("aria-pressed", String(buttonIndex === index)));
}

function selectImageQuery(index) {
  selectedImageQuery = imageDemo.queries[index];
  elements.imageQueryInput.value = selectedImageQuery.text;
  elements.imageSearchButton.disabled = !imageDefaultApi;
  elements.imageDefaultSettingsShortcut.hidden = true;
  elements.imageQueryHint.textContent = "问题已填入。点击右侧发送按钮后，才会执行图片搜索。";
  elements.imageResultSection.hidden = true;
  elements.imagePlanSection.hidden = true;
  document.querySelectorAll("[data-image-query-index]").forEach((button, buttonIndex) => button.setAttribute("aria-pressed", String(buttonIndex === index)));
}

function renderResults(result, target) {
  target.results.innerHTML = result.rows.length > 0
    ? result.rows.map((row, index) => `
      <details class="result-item" ${index === 0 ? "open" : ""}>
        <summary><span class="result-title">#${index + 1} · ${escapeHtml(row.title)}</span><span class="result-distance">距离 ${Number(row.distance).toFixed(4)}</span><span class="chevron" aria-hidden="true">⌄</span></summary>
        <p class="result-content">${escapeHtml(row.content)}</p>
      </details>`).join("")
    : '<p class="empty-result">没有找到结果</p>';
  finishResult(result, target);
}

function renderImageResults(result, target, sourceLookup) {
  target.results.innerHTML = result.rows.length > 0
    ? (() => {
      const row = result.rows[0];
      const source = sourceLookup.get(Number(row.rowid));
      return `
        <article class="image-result-card is-top">
          <div class="image-result-preview"><img src="${escapeHtml(source?.dataUrl || "")}" alt="${escapeHtml(source?.name || row.title)}"></div>
          <div class="image-result-copy"><span class="image-result-rank">最匹配</span><small>距离 ${Number(row.distance).toFixed(4)} · 越小越相似</small></div>
        </article>
        <p class="image-result-note">已从 ${Math.min(result.rows.length, 5)} 个候选中选出最匹配的一张。</p>`;
    })()
    : '<p class="empty-result">没有找到结果</p>';
  finishResult(result, target);
}

function finishResult(result, target) {
  target.time.textContent = `${Number(result.elapsedMs).toFixed(2)} ms`;
  target.plan.textContent = result.plan || "未返回执行计划";
  target.resultSection.hidden = false;
  target.planSection.hidden = false;
  target.resultSection.scrollIntoView({ behavior: "smooth", block: "start" });
}

function searchLocal(api, vector, k) {
  const raw = api.search(JSON.stringify(vector), k);
  if (!raw) throw new Error(api.lastError() || "本机向量搜索失败");
  const result = JSON.parse(raw);
  if (!result.ok) throw new Error("本机向量搜索失败");
  return result;
}

function findBreak(source, start, end, minimum) {
  const strong = new Set(["\n", "。", "！", "？", ".", "!", "?"]);
  for (let index = end - 1; index >= minimum; index -= 1) if (strong.has(source[index])) return index + 1;
  for (let index = end - 1; index >= minimum; index -= 1) if (/\s/.test(source[index])) return index + 1;
  return end;
}

function splitText(source, length, overlap) {
  const text = source.trim();
  if (!text) return [];
  if (text.length > 200000) throw new Error("文本不能超过 20 万个字符");
  if (!Number.isInteger(length) || length < 100 || length > 2000) throw new Error("每段长度需要在 100 到 2000 之间");
  if (!Number.isInteger(overlap) || overlap < 0 || overlap >= length) throw new Error("重叠长度必须小于每段长度");
  const chunks = [];
  let start = 0;
  while (start < text.length) {
    let end = Math.min(start + length, text.length);
    if (end < text.length) end = findBreak(text, start, end, start + Math.floor(length * 0.6));
    const value = text.slice(start, end).trim();
    if (value) chunks.push(value);
    if (end >= text.length) break;
    start = Math.max(start + 1, end - overlap);
    while (start < text.length && /\s/.test(text[start])) start += 1;
  }
  return chunks;
}

function currentIndexSignature() {
  const settings = currentSettings();
  return JSON.stringify([elements.sourceText.value.trim(), Number(elements.chunkLength.value), Number(elements.overlapLength.value), settings.endpoint, settings.model]);
}

function currentImageIndexSignature() {
  const settings = currentImageSettings();
  return JSON.stringify([customImageItems.map(item => [item.id, item.name]), settings.endpoint, settings.model]);
}

function hasFreshCustomIndex() { return Boolean(customApi && generatedSignature === currentIndexSignature()); }
function hasFreshCustomImageIndex() { return Boolean(customImageApi && customImageGeneratedSignature === currentImageIndexSignature()); }

function updateCustomSearchButton() {
  elements.customSearchButton.disabled = customBusy || !hasFreshCustomIndex() || !elements.customQueryInput.value.trim();
}

function updateCustomImageButtons() {
  elements.generateImageVectorsButton.disabled = customImageBusy || customImageItems.length === 0 || !runtimeModule;
  elements.customImageSearchButton.disabled = customImageBusy || !hasFreshCustomImageIndex() || !elements.customImageQueryInput.value.trim();
}

function markCustomIndexStale() {
  if (customApi && !hasFreshCustomIndex()) {
    setCustomStatus("文本或模型已更改，需要重新生成向量");
    elements.customDimensions.textContent = "需要重新生成向量";
  }
  updateCustomSearchButton();
}

function markCustomImageIndexStale() {
  if (customImageApi && !hasFreshCustomImageIndex()) {
    setCustomImageStatus("图片或模型已更改，需要重新生成向量");
    elements.customImageDimensions.textContent = "需要重新生成向量";
  }
  updateCustomImageButtons();
}

function renderCustomChunks() {
  elements.customChunkPreview.innerHTML = customChunks.map((chunk, index) => `
    <details class="chunk-item" ${index === 0 ? "open" : ""}>
      <summary><span class="chunk-title">分片 ${index + 1}</span><span class="chunk-size">${chunk.length} 字</span><span class="chevron" aria-hidden="true">⌄</span></summary>
      <p class="chunk-content">${escapeHtml(chunk)}</p>
    </details>`).join("");
}

function updateChunkPreview() {
  const source = elements.sourceText.value;
  elements.sourceCount.textContent = `${source.length.toLocaleString("zh-CN")} / 200,000 字`;
  elements.toggleEditorButton.hidden = !source.trim();
  elements.overlapLength.max = String(Math.max(0, Number(elements.chunkLength.value) - 1));
  try {
    customChunks = splitText(source, Number(elements.chunkLength.value), Number(elements.overlapLength.value));
    renderCustomChunks();
    elements.chunkPreviewSummary.textContent = customChunks.length ? `已分成 ${customChunks.length} 段，生成前可逐段检查` : "输入文本后显示分片";
    elements.toggleChunkPreview.hidden = customChunks.length === 0;
    elements.generateVectorsButton.disabled = customBusy || customChunks.length === 0 || !runtimeModule;
    elements.generateHint.textContent = customChunks.length ? `准备为 ${customChunks.length} 个分片生成向量` : "生成后会建立一个新的浏览器内 SQLite 索引";
  } catch (error) {
    customChunks = [];
    elements.customChunkPreview.innerHTML = "";
    elements.chunkPreviewSummary.textContent = error instanceof Error ? error.message : String(error);
    elements.toggleChunkPreview.hidden = true;
    elements.generateVectorsButton.disabled = true;
  }
  markCustomIndexStale();
}

function setEditorCollapsed(collapsed) {
  const hasText = Boolean(elements.sourceText.value.trim());
  const shouldCollapse = collapsed && hasText;
  elements.sourceEditorWrap.hidden = shouldCollapse;
  elements.sourceCollapsed.hidden = !shouldCollapse;
  elements.toggleEditorButton.hidden = !hasText;
  elements.toggleEditorButton.textContent = shouldCollapse ? "展开输入框" : "收起输入框";
  const value = elements.sourceText.value.trim();
  elements.sourceCollapsedText.textContent = `${value.slice(0, 100)}${value.length > 100 ? "…" : ""}`;
}

function setCustomBusy(busy) {
  customBusy = busy;
  elements.generateVectorsButton.disabled = busy || customChunks.length === 0 || !runtimeModule;
  elements.testEmbeddingButton.disabled = busy;
  elements.saveSettingsButton.disabled = busy;
  elements.customQueryInput.disabled = busy;
  updateCustomSearchButton();
}

function setCustomImageBusy(busy) {
  customImageBusy = busy;
  elements.imageFileInput.disabled = busy;
  elements.fillSampleImages.disabled = busy;
  elements.testImageEmbeddingButton.disabled = busy;
  elements.saveImageSettingsButton.disabled = busy;
  elements.customImageQueryInput.disabled = busy;
  updateCustomImageButtons();
}

function renderCustomImageGallery() {
  elements.selectedImageCount.textContent = `${customImageItems.length} 张`;
  elements.customImageGallery.innerHTML = customImageItems.length
    ? customImageItems.map((item, index) => `
      <article class="custom-image-item">
        <div class="image-frame"><img src="${escapeHtml(item.dataUrl)}" alt="${escapeHtml(item.name)}"></div>
        <div><strong>${escapeHtml(item.name)}</strong><small>图片 ${index + 1}</small></div>
        <button class="remove-image" type="button" data-remove-image="${escapeHtml(item.id)}" aria-label="删除 ${escapeHtml(item.name)}">×</button>
      </article>`).join("")
    : '<div class="image-empty-state"><strong>还没有图片</strong><span>选择本机图片，或先用内置图片体验完整流程。</span></div>';
  elements.customImageGenerateHint.textContent = customImageItems.length
    ? `准备为 ${customImageItems.length} 张图片生成向量`
    : "选择图片后，先生成向量并建立独立的 L2 索引";
  markCustomImageIndexStale();
}

function readFileAsDataUrl(file) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(String(reader.result));
    reader.onerror = () => reject(new Error(`无法读取图片 ${file.name}`));
    reader.readAsDataURL(file);
  });
}

function loadImage(dataUrl, name) {
  return new Promise((resolve, reject) => {
    const image = new Image();
    image.onload = () => resolve(image);
    image.onerror = () => reject(new Error(`浏览器无法解码 ${name}，请改用 JPEG、PNG 或 WebP`));
    image.src = dataUrl;
  });
}

function canvasToDataUrl(canvas) {
  return new Promise((resolve, reject) => {
    canvas.toBlob(blob => {
      if (!blob) { reject(new Error("图片压缩失败")); return; }
      blobToDataUrl(blob).then(resolve, reject);
    }, "image/jpeg", 0.86);
  });
}

async function normalizeImageFile(file) {
  if (!file.type.startsWith("image/")) throw new Error(`${file.name} 不是图片文件`);
  if (file.size > MAX_IMAGE_BYTES) throw new Error(`${file.name} 超过 12 MB`);
  const input = await readFileAsDataUrl(file);
  const image = await loadImage(input, file.name);
  const scale = Math.min(1, 1600 / Math.max(image.naturalWidth, image.naturalHeight));
  const canvas = document.createElement("canvas");
  canvas.width = Math.max(1, Math.round(image.naturalWidth * scale));
  canvas.height = Math.max(1, Math.round(image.naturalHeight * scale));
  const context = canvas.getContext("2d", { alpha: false });
  if (!context) throw new Error("浏览器无法处理图片");
  context.fillStyle = "#ffffff";
  context.fillRect(0, 0, canvas.width, canvas.height);
  context.drawImage(image, 0, 0, canvas.width, canvas.height);
  return {
    id: globalThis.crypto?.randomUUID?.() ?? `${Date.now()}-${Math.random()}`,
    name: file.name.replace(/\.[^.]+$/, "") || "未命名图片",
    dataUrl: await canvasToDataUrl(canvas)
  };
}

function defaultImageLookup() {
  return new Map(imageDemo.images.map((item, index) => [index + 1, { name: imageTitle(item.resource), dataUrl: imageSources[item.resource] }]));
}

function customImageLookup() {
  return new Map(customImageItems.map((item, index) => [index + 1, item]));
}

async function initialize() {
  try {
    setRuntimeStatus("正在载入真实 WASM");
    elements.defaultStatus.textContent = "正在读取文档和默认向量…";
    elements.imageDefaultStatus.textContent = "正在读取内置图片和默认向量…";
    const loaded = await loadPageAssets();
    demo = loaded.demo;
    imageDemo = loaded.imageDemo;
    documentText = loaded.documentText;
    imageSources = loaded.imageSources;
    runtimeModule = await loaded.createModule();

    elements.documentPreview.textContent = `${documentText.slice(0, 100)}${documentText.length > 100 ? "…" : ""}`;
    renderQuestions();
    renderImageQuestions();
    renderDefaultChunks();
    renderDefaultImageGallery();
    renderCustomImageGallery();
    updateChunkPreview();

    const textApi = createApi(runtimeModule, "bundled_text");
    if (textApi.reset(demo.dimensions) !== 0) throw new Error(textApi.lastError());
    for (const chunk of demo.chunks) {
      const rc = textApi.insert(chunk.id, JSON.stringify(chunk.embedding), `文本片段 ${chunk.id}`, chunk.text);
      if (rc !== 0) throw new Error(textApi.lastError());
    }
    defaultApi = textApi;
    elements.chunkCount.textContent = `${textApi.count()} 段`;
    elements.dimensions.textContent = `${demo.dimensions.toLocaleString("zh-CN")} 维`;
    elements.defaultStatus.textContent = `${textApi.count()} 个分片已建立本机向量索引`;

    const imageApi = createApi(runtimeModule, "bundled_media");
    if (imageApi.resetWithMetric(imageDemo.dimensions, "l2") !== 0) throw new Error(imageApi.lastError());
    for (let index = 0; index < imageDemo.images.length; index += 1) {
      const item = imageDemo.images[index];
      const rc = imageApi.insert(index + 1, JSON.stringify(item.embedding), imageTitle(item.resource), "内置图片");
      if (rc !== 0) throw new Error(imageApi.lastError());
    }
    imageDefaultApi = imageApi;
    elements.imageCount.textContent = `${imageApi.count()} 张`;
    elements.imageDimensions.textContent = `${imageDemo.dimensions.toLocaleString("zh-CN")} 维`;
    elements.imageDefaultStatus.textContent = `${imageApi.count()} 张图片已建立本机 L2 向量索引`;
    elements.versionText.textContent = imageApi.version();
    setRuntimeStatus("WASM 已就绪", "ready");
  } catch (error) {
    console.error(error);
    setRuntimeStatus("载入失败", "error");
    elements.defaultStatus.textContent = "默认索引载入失败";
    elements.imageDefaultStatus.textContent = "默认图片索引载入失败";
    elements.queryHint.textContent = error instanceof Error ? error.message : String(error);
    elements.imageQueryHint.textContent = error instanceof Error ? error.message : String(error);
    elements.generateHint.textContent = "WASM 载入失败，暂时不能建立自定义索引";
    elements.customImageGenerateHint.textContent = "WASM 载入失败，暂时不能建立图片索引";
  }
}

elements.endpointInput.value = safeRead("localStorage", STORAGE_KEYS.endpoint, DEFAULT_ENDPOINT);
elements.modelInput.value = safeRead("localStorage", STORAGE_KEYS.model, DEFAULT_MODEL);
elements.apiKeyInput.value = safeRead("sessionStorage", STORAGE_KEYS.apiKey);
elements.imageEndpointInput.value = safeRead("localStorage", STORAGE_KEYS.imageEndpoint, DEFAULT_IMAGE_ENDPOINT);
elements.imageModelInput.value = safeRead("localStorage", STORAGE_KEYS.imageModel, DEFAULT_IMAGE_MODEL);
elements.imageApiKeyInput.value = safeRead("sessionStorage", STORAGE_KEYS.imageApiKey);
updateSettingsSummary();
updateImageSettingsSummary();

document.querySelector(".mode-switch").addEventListener("click", event => {
  const button = event.target.closest("[data-mode]");
  if (button) activateView(button.dataset.mode, currentView.source);
});
document.querySelector(".tab-switch").addEventListener("click", event => {
  const button = event.target.closest("[data-source]");
  if (button) activateView(currentView.mode, button.dataset.source);
});

elements.presetQuestions.addEventListener("click", event => {
  const button = event.target.closest("[data-query-index]");
  if (button) selectQuery(Number(button.dataset.queryIndex));
});
elements.imageQuestions.addEventListener("click", event => {
  const button = event.target.closest("[data-image-query-index]");
  if (button) selectImageQuery(Number(button.dataset.imageQueryIndex));
});

elements.queryInput.addEventListener("input", () => {
  const value = elements.queryInput.value.trim();
  const index = demo?.queries.findIndex(query => query.text === value) ?? -1;
  selectedQuery = index >= 0 ? demo.queries[index] : null;
  elements.searchButton.disabled = !defaultApi || !value;
  elements.defaultSettingsShortcut.hidden = Boolean(selectedQuery);
  elements.queryHint.textContent = !value
    ? "预设问题使用内置向量；新问题需要先设置 Embedding API。"
    : selectedQuery ? "这是预设问题。点击发送后直接使用内置问题向量，不需要 API Key。" : "这是新问题。点击发送后会使用你设置的 Embedding API 生成查询向量。";
  document.querySelectorAll("[data-query-index]").forEach((button, buttonIndex) => button.setAttribute("aria-pressed", String(buttonIndex === index)));
});

elements.imageQueryInput.addEventListener("input", () => {
  const value = elements.imageQueryInput.value.trim();
  const index = imageDemo?.queries.findIndex(query => query.text === value) ?? -1;
  selectedImageQuery = index >= 0 ? imageDemo.queries[index] : null;
  elements.imageSearchButton.disabled = !imageDefaultApi || !value;
  elements.imageDefaultSettingsShortcut.hidden = Boolean(selectedImageQuery);
  elements.imageQueryHint.textContent = !value
    ? "预设问题使用内置向量；新问题需要先设置图片 Embedding。"
    : selectedImageQuery ? "这是预设问题。点击发送后直接使用内置问题向量，不需要 API Key。" : "这是新问题。点击发送后会使用你设置的图片 Embedding 生成查询向量。";
  document.querySelectorAll("[data-image-query-index]").forEach((button, buttonIndex) => button.setAttribute("aria-pressed", String(buttonIndex === index)));
});

elements.searchForm.addEventListener("submit", async event => {
  event.preventDefault();
  const text = elements.queryInput.value.trim();
  if (!text || !defaultApi) return;
  elements.searchButton.disabled = true;
  setRuntimeStatus(selectedQuery ? "正在本机搜索" : "正在生成问题向量");
  try {
    const vector = selectedQuery ? selectedQuery.embedding : (await embedBatch([text]))[0];
    if (vector.length !== demo.dimensions) throw new Error(`问题向量是 ${vector.length} 维，默认示例需要 ${demo.dimensions} 维，请更换模型`);
    const result = searchLocal(defaultApi, vector, 5);
    renderResults(result, { results: elements.results, time: elements.queryTime, plan: elements.queryPlan, resultSection: elements.resultSection, planSection: elements.planSection });
    elements.queryHint.textContent = selectedQuery ? "已使用内置问题向量完成本机搜索。" : "问题向量由所设 API 生成，SQLite 搜索已在本机完成。";
    setRuntimeStatus("本机搜索完成", "ready");
  } catch (error) {
    setRuntimeStatus("查询失败", "error");
    elements.queryHint.textContent = error instanceof Error ? error.message : String(error);
  } finally { elements.searchButton.disabled = !elements.queryInput.value.trim(); }
});

elements.imageSearchForm.addEventListener("submit", async event => {
  event.preventDefault();
  const text = elements.imageQueryInput.value.trim();
  if (!text || !imageDefaultApi) return;
  elements.imageSearchButton.disabled = true;
  setRuntimeStatus(selectedImageQuery ? "正在本机搜索图片" : "正在生成问题向量");
  try {
    const vector = selectedImageQuery ? selectedImageQuery.embedding : (await embedImageContents([{ text }]))[0];
    if (vector.length !== imageDemo.dimensions) throw new Error(`问题向量是 ${vector.length} 维，默认图片示例需要 ${imageDemo.dimensions} 维，请更换模型`);
    const result = searchLocal(imageDefaultApi, vector, 5);
    renderImageResults(result, { results: elements.imageResults, time: elements.imageQueryTime, plan: elements.imageQueryPlan, resultSection: elements.imageResultSection, planSection: elements.imagePlanSection }, defaultImageLookup());
    elements.imageQueryHint.textContent = selectedImageQuery ? "已使用内置问题向量完成本机图片搜索。" : "问题向量由所设 API 生成，SQLite 图片搜索已在本机完成。";
    setRuntimeStatus("图片搜索完成", "ready");
  } catch (error) {
    setRuntimeStatus("图片查询失败", "error");
    elements.imageQueryHint.textContent = error instanceof Error ? error.message : String(error);
  } finally { elements.imageSearchButton.disabled = !elements.imageQueryInput.value.trim(); }
});

elements.defaultSettingsShortcut.addEventListener("click", () => {
  activateView("text", "custom");
  elements.settingsCard.open = true;
  elements.endpointInput.focus();
});
elements.imageDefaultSettingsShortcut.addEventListener("click", () => {
  activateView("image", "custom");
  elements.imageSettingsCard.open = true;
  elements.imageEndpointInput.focus();
});

elements.documentPreviewButton.addEventListener("click", () => elements.chunksDialog.showModal());
elements.showChunks.addEventListener("click", () => elements.chunksDialog.showModal());
elements.closeChunks.addEventListener("click", () => elements.chunksDialog.close());
elements.chunksDialog.addEventListener("click", event => { if (event.target === elements.chunksDialog) elements.chunksDialog.close(); });

window.addEventListener("scroll", () => {
  if (scrollFrame) return;
  scrollFrame = requestAnimationFrame(() => { scrollFrame = 0; updateBackToTop(); });
}, { passive: true });
elements.backToTop.addEventListener("click", () => window.scrollTo({ top: 0, behavior: "smooth" }));
updateBackToTop();

[elements.endpointInput, elements.modelInput].forEach(input => input.addEventListener("input", () => {
  updateSettingsSummary(); showSettingsMessage(""); markCustomIndexStale();
}));
elements.apiKeyInput.addEventListener("input", () => showSettingsMessage(""));
elements.saveSettingsButton.addEventListener("click", () => { try { saveSettings(); } catch { /* 已显示具体错误。 */ } });
elements.testEmbeddingButton.addEventListener("click", async () => {
  elements.testEmbeddingButton.disabled = true;
  showSettingsMessage("正在用“你好”测试…");
  try {
    const vector = (await embedBatch(["你好"]))[0];
    safeWrite("localStorage", STORAGE_KEYS.endpoint, elements.endpointInput.value.trim());
    safeWrite("localStorage", STORAGE_KEYS.model, elements.modelInput.value.trim());
    safeWrite("sessionStorage", STORAGE_KEYS.apiKey, elements.apiKeyInput.value.trim());
    showSettingsMessage(`“你好”已生成向量，维度为 ${vector.length}。`, "success");
  } catch (error) { showSettingsMessage(error instanceof Error ? error.message : String(error), "error"); }
  finally { elements.testEmbeddingButton.disabled = customBusy; }
});

[elements.imageEndpointInput, elements.imageModelInput].forEach(input => input.addEventListener("input", () => {
  updateImageSettingsSummary(); showImageSettingsMessage(""); markCustomImageIndexStale();
}));
elements.imageApiKeyInput.addEventListener("input", () => showImageSettingsMessage(""));
elements.saveImageSettingsButton.addEventListener("click", () => { try { saveImageSettings(); } catch { /* 已显示具体错误。 */ } });
elements.testImageEmbeddingButton.addEventListener("click", async () => {
  elements.testImageEmbeddingButton.disabled = true;
  showImageSettingsMessage("正在用内置图片测试…");
  try {
    if (!imageDemo) throw new Error("内置图片还没有载入");
    const vector = (await embedImageContents([{ image: imageSources[imageDemo.images[0].resource] }]))[0];
    showImageSettingsMessage(`内置图片已生成向量，维度为 ${vector.length}。`, "success");
  } catch (error) { showImageSettingsMessage(error instanceof Error ? error.message : String(error), "error"); }
  finally { elements.testImageEmbeddingButton.disabled = customImageBusy; }
});

elements.sourceText.addEventListener("input", updateChunkPreview);
elements.chunkLength.addEventListener("input", updateChunkPreview);
elements.overlapLength.addEventListener("input", updateChunkPreview);
elements.fillSampleText.addEventListener("click", () => {
  if (!documentText) return;
  elements.sourceText.value = documentText;
  setEditorCollapsed(false);
  updateChunkPreview();
  elements.sourceText.focus();
});
elements.toggleEditorButton.addEventListener("click", () => setEditorCollapsed(!elements.sourceEditorWrap.hidden));
elements.sourceCollapsed.addEventListener("click", () => { setEditorCollapsed(false); elements.sourceText.focus(); });
elements.toggleChunkPreview.addEventListener("click", () => {
  const hidden = elements.customChunkPreview.getAttribute("aria-hidden") === "true";
  elements.customChunkPreview.setAttribute("aria-hidden", String(!hidden));
  elements.toggleChunkPreview.textContent = hidden ? "收起预览" : "展开预览";
});

elements.generateVectorsButton.addEventListener("click", async () => {
  if (customBusy || customChunks.length === 0 || !runtimeModule) return;
  setCustomBusy(true);
  elements.customProgress.hidden = false;
  elements.customProgress.value = 0;
  elements.customResultSection.hidden = true;
  elements.customPlanSection.hidden = true;
  setCustomStatus("正在请求 Embedding API…");
  try {
    const settings = saveSettings();
    const vectors = [];
    for (let start = 0; start < customChunks.length; start += 64) {
      const batch = customChunks.slice(start, start + 64);
      vectors.push(...await embedBatch(batch));
      elements.customProgress.value = 0.65 * Math.min(1, (start + batch.length) / customChunks.length);
      setCustomStatus(`已生成 ${vectors.length} / ${customChunks.length} 个向量`);
    }
    const dimensions = vectors[0].length;
    if (vectors.some(vector => vector.length !== dimensions)) throw new Error("Embedding API 分批返回的向量维度不一致");
    setCustomStatus("正在浏览器内建立 SQLite 向量索引…");
    const api = createApi(runtimeModule, "user_text");
    if (api.reset(dimensions) !== 0) throw new Error(api.lastError());
    for (let index = 0; index < customChunks.length; index += 1) {
      const rc = api.insert(index + 1, JSON.stringify(vectors[index]), `自定义分片 ${index + 1}`, customChunks[index]);
      if (rc !== 0) throw new Error(api.lastError());
      elements.customProgress.value = 0.65 + 0.35 * ((index + 1) / customChunks.length);
      if ((index + 1) % 24 === 0) await new Promise(resolve => requestAnimationFrame(resolve));
    }
    customApi = api;
    customDimensions = dimensions;
    generatedSignature = currentIndexSignature();
    elements.customRowCount.textContent = `${api.count()} 个分片`;
    elements.customDimensions.textContent = `${dimensions.toLocaleString("zh-CN")} 维`;
    elements.customProgress.value = 1;
    elements.generateHint.textContent = "向量和 SQLite 索引已在当前浏览器内生成";
    setCustomStatus(`${api.count()} 个分片已建立本机向量索引`);
    elements.settingsCard.open = false;
    setEditorCollapsed(true);
    void settings;
  } catch (error) {
    setCustomStatus("生成失败，请检查设置和文本");
    elements.generateHint.textContent = error instanceof Error ? error.message : String(error);
  } finally {
    setCustomBusy(false);
    window.setTimeout(() => { elements.customProgress.hidden = true; }, 500);
  }
});

elements.customQueryInput.addEventListener("input", updateCustomSearchButton);
elements.customSearchForm.addEventListener("submit", async event => {
  event.preventDefault();
  const query = elements.customQueryInput.value.trim();
  if (!query || !hasFreshCustomIndex() || customBusy) return;
  setCustomBusy(true);
  setCustomStatus("正在生成问题向量…");
  try {
    const vector = (await embedBatch([query]))[0];
    if (vector.length !== customDimensions) throw new Error(`问题向量是 ${vector.length} 维，当前索引是 ${customDimensions} 维`);
    const result = searchLocal(customApi, vector, Math.min(5, customApi.count()));
    renderResults(result, { results: elements.customResults, time: elements.customQueryTime, plan: elements.customQueryPlan, resultSection: elements.customResultSection, planSection: elements.customPlanSection });
    setCustomStatus("问题向量已生成，本机搜索完成");
  } catch (error) { setCustomStatus(error instanceof Error ? error.message : String(error)); }
  finally { setCustomBusy(false); }
});

elements.imageFileInput.addEventListener("change", async () => {
  const files = [...elements.imageFileInput.files];
  elements.imageFileInput.value = "";
  if (!files.length) return;
  if (customImageItems.length + files.length > MAX_IMAGES) {
    elements.customImageGenerateHint.textContent = `最多只能导入 ${MAX_IMAGES} 张图片`;
    return;
  }
  setCustomImageBusy(true);
  setCustomImageStatus("正在读取和压缩图片…");
  try {
    for (let index = 0; index < files.length; index += 1) {
      setCustomImageStatus(`正在处理 ${index + 1} / ${files.length} 张图片`);
      customImageItems.push(await normalizeImageFile(files[index]));
      renderCustomImageGallery();
      await new Promise(resolve => requestAnimationFrame(resolve));
    }
    setCustomImageStatus(`${customImageItems.length} 张图片已准备好`);
  } catch (error) {
    elements.customImageGenerateHint.textContent = error instanceof Error ? error.message : String(error);
    setCustomImageStatus("部分图片无法导入");
  } finally { setCustomImageBusy(false); }
});

elements.fillSampleImages.addEventListener("click", () => {
  if (!imageDemo) return;
  customImageItems = imageDemo.images.map((item, index) => ({
    id: `sample-${index + 1}`,
    name: imageTitle(item.resource),
    dataUrl: imageSources[item.resource]
  }));
  renderCustomImageGallery();
  setCustomImageStatus(`${customImageItems.length} 张内置图片已加入自定义图片库`);
});

elements.customImageGallery.addEventListener("click", event => {
  const button = event.target.closest("[data-remove-image]");
  if (!button || customImageBusy) return;
  customImageItems = customImageItems.filter(item => item.id !== button.dataset.removeImage);
  renderCustomImageGallery();
  setCustomImageStatus(customImageItems.length ? `${customImageItems.length} 张图片已准备好` : "还没有生成自定义图片索引");
});

elements.generateImageVectorsButton.addEventListener("click", async () => {
  if (customImageBusy || customImageItems.length === 0 || !runtimeModule) return;
  setCustomImageBusy(true);
  elements.customImageProgress.hidden = false;
  elements.customImageProgress.value = 0;
  elements.customImageResultSection.hidden = true;
  elements.customImagePlanSection.hidden = true;
  setCustomImageStatus("正在请求图片 Embedding API…");
  try {
    saveImageSettings();
    const vectors = [];
    for (let start = 0; start < customImageItems.length; start += 8) {
      const batch = customImageItems.slice(start, start + 8);
      vectors.push(...await embedImageContents(batch.map(item => ({ image: item.dataUrl }))));
      elements.customImageProgress.value = 0.7 * Math.min(1, (start + batch.length) / customImageItems.length);
      setCustomImageStatus(`已生成 ${vectors.length} / ${customImageItems.length} 个图片向量`);
    }
    const dimensions = vectors[0].length;
    if (vectors.some(vector => vector.length !== dimensions)) throw new Error("图片 API 分批返回的向量维度不一致");
    const api = createApi(runtimeModule, "user_media");
    if (api.resetWithMetric(dimensions, "l2") !== 0) throw new Error(api.lastError());
    for (let index = 0; index < customImageItems.length; index += 1) {
      const item = customImageItems[index];
      const rc = api.insert(index + 1, JSON.stringify(vectors[index]), item.name, "自定义图片");
      if (rc !== 0) throw new Error(api.lastError());
      elements.customImageProgress.value = 0.7 + 0.3 * ((index + 1) / customImageItems.length);
    }
    customImageApi = api;
    customImageDimensions = dimensions;
    customImageGeneratedSignature = currentImageIndexSignature();
    elements.customImageRowCount.textContent = `${api.count()} 张图片`;
    elements.customImageDimensions.textContent = `${dimensions.toLocaleString("zh-CN")} 维`;
    elements.customImageProgress.value = 1;
    elements.customImageGenerateHint.textContent = "图片向量和独立的 SQLite 索引已在当前浏览器内生成";
    setCustomImageStatus(`${api.count()} 张图片已建立本机 L2 向量索引`);
    elements.imageSettingsCard.open = false;
  } catch (error) {
    setCustomImageStatus("生成失败，请检查图片和设置");
    elements.customImageGenerateHint.textContent = error instanceof Error ? error.message : String(error);
  } finally {
    setCustomImageBusy(false);
    window.setTimeout(() => { elements.customImageProgress.hidden = true; }, 500);
  }
});

elements.customImageQueryInput.addEventListener("input", updateCustomImageButtons);
elements.customImageSearchForm.addEventListener("submit", async event => {
  event.preventDefault();
  const query = elements.customImageQueryInput.value.trim();
  if (!query || !hasFreshCustomImageIndex() || customImageBusy) return;
  setCustomImageBusy(true);
  setCustomImageStatus("正在生成问题向量…");
  try {
    const vector = (await embedImageContents([{ text: query }]))[0];
    if (vector.length !== customImageDimensions) throw new Error(`问题向量是 ${vector.length} 维，当前图片索引是 ${customImageDimensions} 维`);
    const result = searchLocal(customImageApi, vector, Math.min(5, customImageApi.count()));
    renderImageResults(result, { results: elements.customImageResults, time: elements.customImageQueryTime, plan: elements.customImageQueryPlan, resultSection: elements.customImageResultSection, planSection: elements.customImagePlanSection }, customImageLookup());
    setCustomImageStatus("问题向量已生成，本机图片搜索完成");
  } catch (error) { setCustomImageStatus(error instanceof Error ? error.message : String(error)); }
  finally { setCustomImageBusy(false); }
});

activateView("text", "default");
initialize();
