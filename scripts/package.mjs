import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import {spawn} from "node:child_process";
import {fileURLToPath} from "node:url";
import {verify} from "./verify.mjs";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const debug = process.argv.includes("--debug");
const configuration = debug ? "Debug" : "Release";
const preset = `windows-clang-${configuration.toLowerCase()}`;
const variant = debug ? "engine-debug" : "engine";
const archive = path.join(root, debug ? "plugin-debug.zip" : "plugin.zip");

function run(command, args, options = {}) {
    return new Promise((resolve, reject) => {
        const child = spawn(command, args, {cwd: options.cwd ?? root, env: options.env ?? process.env, stdio: "inherit"});
        child.on("error", reject);
        child.on("close", (code) => code === 0 ? resolve() : reject(new Error(`${command} exited with ${code}`)));
    });
}

async function exists(file) {
    try { await fs.access(file); return true; } catch { return false; }
}

async function main() {
    await verify();
    if (process.platform !== "win32") throw new Error("This release script currently builds Windows x64 packages only.");
    const vcpkgRoot = path.resolve(process.env.VCPKG_ROOT || path.join(root, "vcpkg"));
    if (!await exists(path.join(vcpkgRoot, "scripts", "buildsystems", "vcpkg.cmake"))) {
        throw new Error("Set VCPKG_ROOT to a vcpkg checkout.");
    }
    const env = {...process.env, VCPKG_ROOT: vcpkgRoot};
    const configureArgs = ["--preset", preset];
    if (process.env.DARTWIC_VCPKG_INSTALLED_DIR) {
        const installed = path.resolve(process.env.DARTWIC_VCPKG_INSTALLED_DIR);
        if (!await exists(path.join(installed, "x64-windows", "share", "nlohmann_json"))) {
            throw new Error("DARTWIC_VCPKG_INSTALLED_DIR does not contain x64-windows dependencies.");
        }
        configureArgs.push("-DVCPKG_MANIFEST_INSTALL=OFF", `-DVCPKG_INSTALLED_DIR=${installed}`);
    }
    await run("cmake", configureArgs, {env});
    await run("cmake", ["--build", "--preset", `build-${preset}`, "--target", "copy_engine_plugin"], {env});

    const output = path.join(root, "plugin", variant, "fprime_bridge");
    for (const required of ["plugin.json", path.join("bin", "fprime_bridge.dll"),
        ...(debug ? [path.join("bin", "fprime_bridge.pdb")] : [])]) {
        if (!await exists(path.join(output, required))) throw new Error(`Build did not produce ${required}.`);
    }
    const stage = await fs.mkdtemp(path.join(os.tmpdir(), "fprime-dartwic-package-"));
    try {
        const stagePlugin = path.join(stage, "plugin", variant, "fprime_bridge");
        await fs.mkdir(path.dirname(stagePlugin), {recursive: true});
        await fs.cp(output, stagePlugin, {recursive: true});
        await run("powershell.exe", ["-NoProfile", "-Command",
            `Compress-Archive -LiteralPath 'plugin' -DestinationPath '${archive.replaceAll("'", "''")}' -Force`], {cwd: stage});
    } finally {
        if (!stage.startsWith(path.join(os.tmpdir(), "fprime-dartwic-package-"))) {
            throw new Error("Refusing to remove an unexpected package staging path.");
        }
        await fs.rm(stage, {recursive: true, force: true});
    }
    console.log(`Packaged ${configuration} engine plugin: ${archive}`);
}

await main();
