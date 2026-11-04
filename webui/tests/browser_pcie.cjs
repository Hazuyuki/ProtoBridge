/* Optional real-browser/native-simulator check; run against a separate data dir.
 * NODE_PATH=/path/to/node_modules node webui/tests/browser_pcie.cjs [base URL]
 */
const assert = require("node:assert/strict");
const { chromium } = require("playwright");
const base = (process.argv[2] || "http://127.0.0.1:8081").replace(/\/$/, "");

(async () => {
  const browser = await chromium.launch({ headless: true, args: ["--no-sandbox"] });
  try {
    const page = await browser.newPage({ locale: "zh-CN", viewport: { width: 1440, height: 1000 } });
    const errors = [];
    page.on("pageerror", error => errors.push(error.message));
    const ready = () => page.waitForFunction(() => !document.querySelector("#run-button").disabled);
    await page.goto(base);
    await ready();
    assert.equal(await page.locator("html").getAttribute("lang"), "en");
    await page.locator('[data-scene="multinode"]').click();
    await page.locator("#config-select").selectOption("rdma-read.cfg");
    await ready();
    await page.locator("#options-pcie").evaluate(node => node.open = true);
    assert.equal(await page.locator("#opt-pcieModel").isVisible(), true);
    assert.equal(await page.locator("#opt-pcieTags").isVisible(), false);
    await page.locator("#opt-pcieModel").selectOption("tlp");
    await ready();
    assert.equal(await page.locator("#opt-pcieTags").isVisible(), true);
    await page.locator("#opt-pcieTags").fill("2");
    await page.locator("#opt-pcieGeneration").selectOption("5");
    await page.locator("#data-size").fill("9");
    await page.locator("#data-unit").selectOption("1024");
    await ready();
    await page.locator('[data-language="zh"]').click();
    assert.match(await page.locator("#options-pcie summary").innerText(), /本机 PCIe/);
    assert.equal(await page.locator("#opt-pcieTags").inputValue(), "2");
    assert.equal(await page.locator("#opt-pcieModel").inputValue(), "tlp");
    await page.locator("#options-rdma").evaluate(node => node.open = true);
    assert.equal(await page.locator("#opt-rdmaPcieReadGbps").isVisible(), false);
    const submitted = page.waitForResponse(r => r.url() === base + "/api/runs" && r.request().method() === "POST");
    await page.locator("#run-button").click();
    const response = await submitted;
    assert.equal(response.status(), 202);
    const { id } = await response.json();
    let job;
    for (let i = 0; i < 100; ++i) {
      job = await (await page.request.get(`${base}/api/runs/${id}`)).json();
      if (["completed", "failed", "cancelled"].includes(job.status)) break;
      await page.waitForTimeout(100);
    }
    assert.equal(job.status, "completed", JSON.stringify(job));
    assert.equal(job.result.pcieModel, "tlp");
    assert.equal(job.result.pcieGeneration, 5);
    assert.ok(job.result.pciePeakOutstandingReads <= 2);
    const trace = await page.request.get(`${base}/api/runs/${id}/artifacts/pcie.csv`);
    assert.equal(trace.status(), 200);
    assert.match(await trace.text(), /CplD/);
    await page.locator('[data-language="en"]').click();
    assert.match(await page.locator("#options-pcie summary").innerText(), /Local PCIe/);
    await page.locator("#opt-pcieModel").selectOption("bandwidth");
    await ready();
    assert.equal(await page.locator("#opt-pcieTags").isVisible(), false);
    assert.equal(await page.locator("#opt-rdmaPcieReadGbps").isVisible(), true);
    await page.setViewportSize({ width: 390, height: 844 });
    assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth + 1));
    assert.deepEqual(errors, []);
    console.log("PASS PCIe model controls, both languages, native Gen5/tag limit, trace download, model switch and mobile layout");
  } finally {
    await browser.close();
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
