import crypto from "node:crypto";
import fs from "node:fs/promises";
import path from "node:path";
import {fileURLToPath} from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

async function filesWithin(directory, current = directory) {
    const files = [];
    for (const entry of await fs.readdir(current, {withFileTypes: true})) {
        const entryPath = path.join(current, entry.name);
        if (entry.isDirectory()) files.push(...await filesWithin(directory, entryPath));
        else if (entry.isFile()) files.push(path.relative(directory, entryPath).replaceAll("\\", "/"));
    }
    return files.sort((left, right) => left.localeCompare(right));
}

async function hashDirectory(directory) {
    const hash = crypto.createHash("sha256");
    for (const relative of await filesWithin(directory)) {
        hash.update(relative);
        hash.update("\0");
        hash.update((await fs.readFile(path.join(directory, relative), "utf8"))
            .replaceAll("\r\n", "\n").replaceAll("\r", "\n"));
        hash.update("\0");
    }
    return hash.digest("hex");
}

export async function verify() {
    const manifest = JSON.parse(await fs.readFile(path.join(root, "plugin.json"), "utf8"));
    const packageJson = JSON.parse(await fs.readFile(path.join(root, "package.json"), "utf8"));
    const lock = JSON.parse(await fs.readFile(path.join(root, "sdk-lock.json"), "utf8"));
    if (manifest.id !== "fprime_bridge" || manifest.contains_engine_plugin !== true
        || manifest.contains_interface_plugin !== false || manifest.version !== packageJson.version) {
        throw new Error("Plugin manifest and package version do not match the F Prime engine plugin.");
    }
    const actual = await hashDirectory(path.join(root, "engine", "include", "sdk"));
    if (actual !== lock.engineSdkSha256) throw new Error("Bundled Engine SDK differs from sdk-lock.json.");
    if (await hashDirectory(path.join(root, "vendor")) !== lock.peerDependenciesSha256)
        throw new Error("Bundled TEMPEST/Engine Protocol sources differ from sdk-lock.json.");
    for (const relative of ["CMakeLists.txt", "engine/src/FprimeBridgePlugin.cpp",
        "engine/src/FprimeTransport.cpp", "engine/src/FprimeDictionary.cpp",
        "test/WindowsBridgeTest.cpp"]) {
        await fs.access(path.join(root, relative));
    }
    console.log(`Verified F Prime plugin source and bundled SDK (${manifest.version}).`);
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    if (process.argv.includes("--write-lock")) {
        const lock = {
            engineSdkSha256: await hashDirectory(path.join(root, "engine/include/sdk")),
            peerDependenciesSha256: await hashDirectory(path.join(root, "vendor")),
        };
        await fs.writeFile(path.join(root, "sdk-lock.json"), JSON.stringify(lock, null, 2) + "\n");
    }
    await verify();
}
