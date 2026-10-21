import {getPluginPlatform, packagePluginRelease} from "./plugin-release.mjs";
import fs from "node:fs/promises";
import path from "node:path";
import {spawn} from "node:child_process";
import {fileURLToPath} from "node:url";
import {verify} from "./verify.mjs";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");


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
    getPluginPlatform();
    if (process.platform !== "win32") throw new Error("This release script currently builds Windows x64 packages only.");
    const vcpkgRoot = path.resolve(process.env.VCPKG_ROOT || path.join(root, "vcpkg"));
    if (!await exists(path.join(vcpkgRoot, "scripts", "buildsystems", "vcpkg.cmake"))) {
        throw new Error("Set VCPKG_ROOT to a vcpkg checkout.");
    }
    const env = {...process.env, VCPKG_ROOT: vcpkgRoot};
    for (const configuration of ["Release", "Debug"]) {
        const preset = `windows-clang-${configuration.toLowerCase()}`;
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
    }
    await packagePluginRelease(root);
}

await main();
