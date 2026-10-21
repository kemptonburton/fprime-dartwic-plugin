import fs from "node:fs/promises";
import path from "node:path";
import os from "node:os";
import crypto from "node:crypto";
import {spawn} from "node:child_process";

export function getPluginPlatform(platform = process.platform, arch = process.arch) {
  const key = `${platform}-${arch}`;
  if (!["win32-x64", "linux-x64"].includes(key)) throw new Error(`Unsupported plugin build host: ${key}. Build on Windows x64 or Linux x64.`);
  return key;
}

async function run(command, args, cwd) {
  await new Promise((resolve, reject) => {
    const child = spawn(command, args, {cwd, stdio: "inherit", windowsHide: true});
    child.on("error", reject);
    child.on("close", code => code === 0 ? resolve() : reject(new Error(`${command} exited with ${code}`)));
  });
}

export async function verifyPluginOutput(root, platform = getPluginPlatform()) {
  const manifest = JSON.parse(await fs.readFile(path.join(root, "plugin.json"), "utf8"));
  const packageJson = JSON.parse(await fs.readFile(path.join(root, "package.json"), "utf8"));
  if (manifest.version !== packageJson.version) throw new Error("package.json and plugin.json versions must match.");
  if (!manifest.id || !manifest.name || !manifest.version
      || typeof manifest.contains_engine_plugin !== "boolean" || typeof manifest.contains_interface_plugin !== "boolean"
      || (!manifest.contains_engine_plugin && !manifest.contains_interface_plugin)
      || (manifest.contains_engine_plugin && !manifest.minEngineVersion)
      || (manifest.contains_interface_plugin && !manifest.minInterfaceVersion)) throw new Error("Invalid root plugin.json; see the README release contract.");
  if (!/^[A-Za-z0-9_-]+$/.test(manifest.id)) throw new Error("Plugin ID must be a safe directory name.");
  const sides = [ ...(manifest.contains_engine_plugin ? ["engine", "engine-debug"] : []), ...(manifest.contains_interface_plugin ? ["interface"] : []) ];
  for (const side of sides) {
    const output = path.join(root, "plugin", side, manifest.id);
    const copy = JSON.parse(await fs.readFile(path.join(output, "plugin.json"), "utf8"));
    for (const field of ["id", "name", "version", "minEngineVersion", "minInterfaceVersion", "contains_engine_plugin", "contains_interface_plugin"]) {
      if (copy[field] !== manifest[field]) throw new Error(`${side}/plugin.json differs from the root manifest (${field}). Rebuild the package.`);
    }
    if (side === "interface") await fs.access(path.join(output, "ui", "index.js"));
    else {
      const library = platform === "win32-x64" ? `${manifest.id}.dll` : `lib${manifest.id}.so`;
      const bytes = await fs.readFile(path.join(output, "bin", library));
      const nativePlatform = bytes.subarray(0, 2).toString() === "MZ"
        ? (bytes.readUInt16LE(bytes.readUInt32LE(60) + 4) === 0x8664 ? "win32-x64" : "unknown")
        : (bytes.subarray(0, 4).equals(Buffer.from([127,69,76,70])) && bytes[4] === 2 && bytes[5] === 1 && bytes.readUInt16LE(18) === 62 ? "linux-x64" : "unknown");
      if (nativePlatform !== platform) throw new Error(`${side}/${library} is not a ${platform} binary.`);
      if (side === "engine-debug" && platform === "win32-x64") await fs.access(path.join(output, "bin", `${manifest.id}.pdb`));
    }
  }
  return {manifest, sides};
}

export async function packagePluginRelease(root, platforms = [getPluginPlatform()]) {
  if (!platforms.length || platforms.some(platform => !["win32-x64", "linux-x64"].includes(platform))) throw new Error("Choose win32-x64 and/or linux-x64.");
  const manifest = JSON.parse(await fs.readFile(path.join(root, "plugin.json"), "utf8"));
  if (manifest.contains_engine_plugin && (platforms.length !== 1 || platforms[0] !== getPluginPlatform())) throw new Error("Build native plugins on their target platform; never relabel a native ZIP.");
  const stage = await fs.mkdtemp(path.join(os.tmpdir(), "dartwic-plugin-release-"));
  const filenames = ["plugin.json"];
  try {
    const {sides} = await verifyPluginOutput(root, platforms[0]);
    for (const side of sides) {
      const target = path.join(stage, "plugin", side, manifest.id);
      await fs.mkdir(path.dirname(target), {recursive: true});
      await fs.cp(path.join(root, "plugin", side, manifest.id), target, {recursive: true});
    }
    const files = path.join(root, "plugin", "files");
    if (await fs.stat(files).catch(() => null)) await fs.cp(files, path.join(stage, "plugin", "files"), {recursive: true});
    for (const platform of platforms) {
      const name = `plugin-${platform}.zip`;
      const archive = path.join(root, name);
      await fs.rm(archive, {force: true});
      if (process.platform === "win32") await run("powershell.exe", ["-NoProfile", "-Command", `Compress-Archive -LiteralPath 'plugin' -DestinationPath '${archive.replaceAll("'", "''")}' -Force`], stage);
      else await run("zip", ["-qr", archive, "plugin"], stage);
      filenames.push(name);
    }
    for (const legacy of ["plugin.zip", "plugin-debug.zip"]) await fs.rm(path.join(root, legacy), {force: true});
    const receipt = {version: manifest.version, platforms, engineVariants: manifest.contains_engine_plugin ? ["engine", "engine-debug"] : [], files: {}};
    for (const name of filenames) {
      const bytes = await fs.readFile(path.join(root, name));
      receipt.files[name] = {sha256: crypto.createHash("sha256").update(bytes).digest("hex"), bytes: bytes.length};
    }
    await fs.writeFile(path.join(root, "release-manifest.json"), JSON.stringify(receipt, null, 2) + "\n");
    console.log(`Upload plugin.json and ${filenames.slice(1).join(" and ")} to the same GitHub release. Checksums: release-manifest.json.`);
    return receipt;
  } finally {
    if (!stage.startsWith(path.join(os.tmpdir(), "dartwic-plugin-release-"))) throw new Error("Unexpected staging path.");
    await fs.rm(stage, {recursive: true, force: true});
  }
}
