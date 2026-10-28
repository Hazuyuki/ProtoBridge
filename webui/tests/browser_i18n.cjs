/* Optional real-browser / real-simulator check. Use a separate WebUI data dir.
 * NODE_PATH=/path/to/node_modules node webui/tests/browser_i18n.cjs [base URL]
 */
const assert = require("node:assert/strict");
const { chromium } = require("playwright");

const base = (process.argv[2] || "http://127.0.0.1:8081").replace(/\/$/, "");
const checks = [];
const pass = (name) => {
  checks.push(name);
  console.log("PASS", name);
};

(async () => {
  const browser = await chromium.launch({
    headless: true,
    args: ["--no-sandbox"],
  });
  try {
    const context = await browser.newContext({
      locale: "zh-CN",
      viewport: { width: 1440, height: 1000 },
    });
    const page = await context.newPage();
    const errors = [];
    let submissions = 0;
    page.on("pageerror", (error) => errors.push(error.message));
    page.on("request", (request) => {
      if (request.url() === base + "/api/runs" && request.method() === "POST")
        submissions++;
    });
    const ready = () =>
      page.waitForFunction(
        () => !document.querySelector("#run-button").disabled,
      );
    const language = async (value) => {
      // A modal makes the header inert. Exercise its language update through
      // the same handler, then verify the still-open modal below.
      if (await page.locator("dialog[open]").count())
        await page
          .locator(`[data-language="${value}"]`)
          .evaluate((button) => button.click());
      else await page.locator(`[data-language="${value}"]`).click();
      assert.equal(
        await page.locator("html").getAttribute("lang"),
        value === "en" ? "en" : "zh-CN",
      );
      assert.equal(
        await page
          .locator(`[data-language="${value}"]`)
          .getAttribute("aria-pressed"),
        "true",
      );
    };
    const inputs = () =>
      page.evaluate(() =>
        [
          ...document.querySelectorAll(
            "#simulation-form input, #simulation-form select, #simulation-form textarea",
          ),
        ]
          .filter((input) => input.type !== "file")
          .map((input) => [
            input.id,
            input.value,
            input.checked,
            input.disabled,
          ]),
      );
    await page.goto(base);
    await ready();
    assert.equal(await page.locator("html").getAttribute("lang"), "en");
    assert.match(
      await page.locator("h1").innerText(),
      /Set up a communication simulation/,
    );
    assert.match(
      await page.locator("#engine-state").innerText(),
      /Simulator ready/,
    );
    assert.equal(await page.locator("#collective-select option").count(), 7);
    pass(
      "English default even with a Chinese browser locale; native choices load",
    );

    await page.locator("#run-name").fill("请求 / 响应");
    await page.locator("#physical-topology").selectOption("leafspine");
    await page.locator("#algorithm-select").selectOption("tree");
    await page.locator(".source-editor summary").click();
    const config = await page.locator("#config-editor").inputValue();
    const profile = await page.locator("#profile-editor").inputValue();
    await page.locator("#config-editor").fill("# 原始配置 Ω\n" + config);
    await page.locator("#profile-editor").fill("# 保留 profile\n" + profile);
    await page.locator("#cli-json").evaluate((input) => {
      input.closest("details").open = true;
    });
    await page.locator("#cli-json").fill('{"bgP2pEnabled": false}');
    await page.locator("#bandwidth").fill("85");
    await ready();
    const before = await inputs();
    const openDetails = await page
      .locator("details[open]")
      .evaluateAll((nodes) => nodes.map((node) => node.id || node.className));
    await language("zh");
    assert.deepEqual(await inputs(), before);
    assert.deepEqual(
      await page
        .locator("details[open]")
        .evaluateAll((nodes) => nodes.map((node) => node.id || node.className)),
      openDetails,
    );
    assert.match(
      await page.locator("#topology-options-title").textContent(),
      /拓扑参数/,
    );
    await language("en");
    assert.deepEqual(await inputs(), before);
    assert.equal(submissions, 0);
    pass(
      "Switching preserves unsaved source, JSON, user names, choices and open sections without submitting",
    );

    await language("zh");
    await page.locator("#cli-json").fill("{");
    await page.waitForFunction(
      () => !document.querySelector("#form-error").hidden,
    );
    assert.match(
      await page.locator("#form-error").textContent(),
      /JSON 格式无效/,
    );
    await language("en");
    assert.match(
      await page.locator("#form-error").textContent(),
      /Invalid JSON/,
    );
    assert.equal(await page.locator("#run-button").isDisabled(), true);
    await page.locator("#cli-json").fill("{}");
    await page.locator("#num-gpus").fill("1");
    await page.waitForFunction(() =>
      document.querySelector("#form-error").textContent.includes("num_gpus"),
    );
    assert.match(
      await page.locator("#form-error").textContent(),
      /Must be at least 2/,
    );
    await language("zh");
    assert.match(
      await page.locator("#form-error").textContent(),
      /必须大于或等于 2/,
    );
    await language("en");
    assert.match(
      await page.locator("#form-error").textContent(),
      /Must be at least 2/,
    );
    await page.locator("#num-gpus").fill("8");
    await page.locator("#config-editor").fill("[op]\ncollective = allreduce");
    await page.waitForFunction(() =>
      document
        .querySelector("#form-error")
        .textContent.includes("nonempty [op]"),
    );
    await language("zh");
    assert.match(await page.locator("#form-error").textContent(), /配置需要/);
    pass(
      "Client and API validation errors switch language and block invalid runs",
    );

    await page.locator("#reset-button").click();
    await ready();
    const run = async (name) => {
      await page.locator("#run-name").fill(name);
      const responsePromise = page.waitForResponse(
        (response) =>
          response.url() === base + "/api/runs" &&
          response.request().method() === "POST",
      );
      await page.locator("#run-button").click();
      const response = await responsePromise;
      assert.equal(response.status(), 202);
      const { id } = await response.json();
      await page.waitForFunction(
        (id) =>
          location.hash.includes(id) &&
          document
            .querySelector("#selected-run-status")
            .classList.contains("completed"),
        id,
        { timeout: 30000 },
      );
      const detail = await context.request.get(base + "/api/runs/" + id);
      return detail.json();
    };
    const chinese = await run("请求 / 响应");
    await page.locator('[data-tab="trace"]').click();
    await page.waitForSelector("#trace-content svg circle");
    const originalSnapshot = await page.locator("#run-snapshot").textContent();
    const pointCount = await page.locator("#trace-content svg circle").count();
    await language("en");
    await page.waitForFunction(() =>
      document
        .querySelector("#trace-content svg")
        .getAttribute("aria-label")
        .includes("Step-completion"),
    );
    assert.equal(
      await page.locator('[data-tab="trace"]').getAttribute("aria-selected"),
      "true",
    );
    assert.equal(
      await page.locator("#trace-content svg circle").count(),
      pointCount,
    );
    assert.equal(
      await page.locator("#run-snapshot").textContent(),
      originalSnapshot,
    );
    assert.match(
      await page.locator("#selected-run-status").textContent(),
      /Completed/,
    );
    assert.match(
      await page.locator("#selected-run-description").textContent(),
      /请求 \/ 响应/,
    );
    const english = await run("English baseline");
    assert.deepEqual(
      { ...chinese.request, name: "" },
      { ...english.request, name: "" },
    );
    assert.equal(chinese.result.simTimeNs, english.result.simTimeNs);
    assert.equal(
      chinese.result.endpointTxPackets,
      english.result.endpointTxPackets,
    );
    pass(
      "Identical native parameters and results in both languages; names, snapshots and trace data stay intact",
    );

    for (const id of [chinese.id, english.id])
      await page.locator(`[data-compare="${id}"]`).check();
    await page.locator("#compare-button").click();
    await language("zh");
    assert.equal(
      await page.locator("#compare-dialog").getAttribute("open"),
      "",
    );
    assert.equal(
      await page.locator("#comparison-chart .comparison-item").count(),
      2,
    );
    assert.equal(await page.locator("[data-compare]:checked").count(), 2);
    await page.keyboard.press("Escape");
    const selectedId = english.id;
    await page.reload();
    await ready();
    await page.waitForFunction(
      (id) =>
        document
          .querySelector("#run-identifier")
          .textContent.includes(id.slice(0, 8)),
      selectedId,
    );
    assert.equal(await page.locator("html").getAttribute("lang"), "zh-CN");
    assert.match(
      await page.locator("#clear-errors").textContent(),
      /设为无错误/,
    );
    await language("en");
    await page.reload();
    await ready();
    assert.equal(await page.locator("html").getAttribute("lang"), "en");
    pass(
      "Comparison selection survives switching; language preference and selected run survive reload",
    );

    for (const value of ["zh", "en"]) {
      await language(value);
      await page.locator('[data-scene="multinode"]').click();
      await ready();
      assert.equal(await page.locator("#inter-topology option").count(), 8);
      await page.locator('[data-scene="stencil"]').click();
      await page.waitForFunction(() =>
        /Transfer stencil|自定义传输/.test(
          document.querySelector("#network-operation").textContent,
        ),
      );
      await ready();
      assert.match(
        await page.locator("#topology-badge").textContent(),
        value === "en" ? /Transfer stencil/ : /自定义传输/,
      );
      await page.locator('[data-scene="collective"]').click();
      await page.waitForFunction(() =>
        document
          .querySelector("#network-operation")
          .textContent.includes("allreduce"),
      );
      await ready();
      await page.setViewportSize({ width: 390, height: 844 });
      assert.equal(
        await page.evaluate(
          () => document.documentElement.scrollWidth > innerWidth,
        ),
        false,
      );
      assert.equal(await page.locator(".language-switch").isVisible(), true);
      await page
        .locator("[data-help]")
        .last()
        .evaluate((button) => button.click());
      assert.equal(await page.locator("#help-dialog").getAttribute("open"), "");
      assert.equal(
        await page.evaluate(
          () => document.documentElement.scrollWidth > innerWidth,
        ),
        false,
      );
      await page.keyboard.press("Escape");
      await page.setViewportSize({ width: 1440, height: 1000 });
    }
    pass(
      "All scenarios, accessible help and 390px layout work in both languages",
    );

    await page.route("**/api/status", (route) => route.abort());
    await page.waitForFunction(() =>
      document
        .querySelector("#connection-error")
        .textContent.includes("Connection lost:"),
    );
    await language("zh");
    assert.equal(await page.locator("#run-button").isDisabled(), true);
    assert.match(await page.locator("#engine-state").textContent(), /连接中断/);
    await language("en");
    assert.equal(await page.locator("#run-button").isDisabled(), true);
    assert.match(
      await page.locator("#connection-error").textContent(),
      /Connection lost:/,
    );
    await page.unroute("**/api/status");
    await ready();
    pass(
      "Switching during a lost connection keeps submission disabled and recovers after reconnecting",
    );

    const blocked = await browser.newContext();
    await blocked.addInitScript(() => {
      Object.defineProperty(window, "localStorage", {
        get() {
          throw new DOMException("Storage blocked", "SecurityError");
        },
      });
    });
    const restrictedPage = await blocked.newPage();
    restrictedPage.on("pageerror", (error) => errors.push(error.message));
    await restrictedPage.goto(base);
    await restrictedPage.waitForFunction(
      () => !document.querySelector("#run-button").disabled,
    );
    assert.equal(
      await restrictedPage.locator("html").getAttribute("lang"),
      "en",
    );
    await restrictedPage.locator('[data-language="zh"]').click();
    assert.equal(
      await restrictedPage.locator("html").getAttribute("lang"),
      "zh-CN",
    );
    await restrictedPage.reload();
    await restrictedPage.waitForFunction(
      () => !document.querySelector("#run-button").disabled,
    );
    assert.equal(
      await restrictedPage.locator("html").getAttribute("lang"),
      "en",
    );
    await blocked.close();
    pass(
      "Blocked local storage supports in-page switching and falls back to English on reload",
    );

    const fallbackPage = await context.newPage();
    fallbackPage.on("pageerror", (error) => errors.push(error.message));
    await fallbackPage.route("**/static/locales.json*", (route) =>
      route.fulfill({ status: 503, body: "unavailable" }),
    );
    await fallbackPage.goto(base);
    await fallbackPage.waitForFunction(
      () => !document.querySelector("#run-button").disabled,
    );
    assert.equal(await fallbackPage.locator("html").getAttribute("lang"), "en");
    assert.equal(
      await fallbackPage.locator('[data-language="zh"]').isDisabled(),
      true,
    );
    assert.match(
      await fallbackPage.locator("#config-description").textContent(),
      /Single-node/,
    );
    await fallbackPage.close();
    pass("Unavailable translation catalog leaves a usable English interface");
    assert.equal(submissions, 2);
    assert.deepEqual(errors, []);
    pass("Only two deliberate submissions and no browser JavaScript errors");
    console.log(`${checks.length} browser checks passed`);
    await context.close();
  } finally {
    await browser.close();
  }
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
