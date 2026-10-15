import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import { pathToFileURL } from 'node:url';

const project = path.resolve(process.argv[2]);
const evidence = path.resolve(process.argv[3]);
fs.mkdirSync(evidence, { recursive: true });
const report = { success: false, project, started_at: new Date().toISOString(), calls: [] };
const serverRoot = path.join(project, '.agent-local/enginewire/server/node_modules');
const log = fs.createWriteStream(path.join(evidence, 'enginewire-stderr.log'));
let client;
try {
  const { Client } = await import(pathToFileURL(path.join(serverRoot, '@modelcontextprotocol/sdk/dist/esm/client/index.js')));
  const { StdioClientTransport } = await import(pathToFileURL(path.join(serverRoot, '@modelcontextprotocol/sdk/dist/esm/client/stdio.js')));
  const transport = new StdioClientTransport({
    command: process.execPath,
    args: [path.join(serverRoot, 'unreal-engine-mcp-server/dist/cli.js')],
    cwd: project,
    env: { ...process.env, UE_PROJECT_PATH: project, LOG_LEVEL: 'warn' },
    stderr: 'pipe',
  });
  transport.stderr?.pipe(log);
  client = new Client({ name: 'uemmo-enginewire-verification', version: '1.0.0' });
  await client.connect(transport);
  report.server = client.getServerVersion();
  const pin = JSON.parse(fs.readFileSync(path.join(project, 'Scripts/EngineWire/version.json'), 'utf8'));
  assert.equal(report.server.version, pin.npmVersion, 'MCP server version differs from the pinned plugin');
  report.tools = (await client.listTools()).tools.map(tool => tool.name);
  assert.deepEqual(report.tools, ['unreal']);

  async function call(args) {
    const reply = await client.callTool({ name: 'unreal', arguments: args }, undefined, { timeout: 120000 });
    const result = reply.structuredContent;
    report.calls.push({ request: args, result });
    assert.ok(!reply.isError && result?.success === true,
      `${args.tool ?? args.capability}.${args.action ?? args.operation}: ${result?.errorCode ?? ''} ${result?.message ?? 'missing structured result'}`);
    console.log(`PASS ${args.tool ?? args.capability}.${args.action ?? args.operation}`);
    return result;
  }

  // Catalog queries do not contact Unreal. This read proves the editor bridge responds.
  await new Promise(resolve => setTimeout(resolve, 2500));
  await call({ operation: 'describe', capability: 'control_actor.list' });
  const actors = await call({ operation: 'execute', tool: 'control_actor', action: 'list', params: { limit: 10 } });
  assert.ok(actors.data?.actors?.length > 0, 'The loaded project level has no actors to inspect');
  report.world = actors.data.worldName;
  const actor = actors.data.actors.find(value => value.location) ?? actors.data.actors[0];
  await call({ operation: 'describe', capability: 'inspect.get_components' });
  const components = await call({ operation: 'execute', tool: 'inspect', action: 'get_components', params: { actorName: actor.name } });
  report.actor = actor.name;
  report.components = components.data?.components;

  await call({ operation: 'describe', capability: 'asset.list' });
  const assets = await call({ operation: 'execute', tool: 'manage_asset', action: 'list', params: { path: '/Game/UEMMO', recursive: true, limit: 10 } });
  assert.ok(assets.data?.assets?.length > 0, 'No project assets returned');
  report.asset_count = assets.data.totalCount ?? assets.data.assets.length;

  await call({ operation: 'describe', capability: 'control_editor.screenshot' });
  const screenshot = await call({ operation: 'execute', tool: 'control_editor', action: 'screenshot', params: {
    filename: 'enginewire-verification', mode: 'editor_viewport', resolution: '1280x720', returnBase64: false, waitForShaders: true,
  } });
  const source = screenshot.data?.screenshotPath;
  assert.ok(source && fs.existsSync(source), 'MCP reported no readable screenshot');
  const image = fs.readFileSync(source);
  assert.ok(image.subarray(0, 8).equals(Buffer.from([137, 80, 78, 71, 13, 10, 26, 10])), 'Screenshot is not a PNG');
  assert.ok(image.length > 1024, 'Screenshot is unexpectedly small');
  report.screenshot = path.join(evidence, 'enginewire-viewport.png');
  fs.writeFileSync(report.screenshot, image);
  report.screenshot_width = image.readUInt32BE(16);
  report.screenshot_height = image.readUInt32BE(20);
  report.success = true;
} catch (error) {
  report.error = String(error);
  console.error(report.error);
} finally {
  await client?.close().catch(() => {});
  log.end();
  report.finished_at = new Date().toISOString();
  fs.writeFileSync(path.join(evidence, 'enginewire-summary.json'), JSON.stringify(report, null, 2));
}
process.exitCode = report.success ? 0 : 1;
