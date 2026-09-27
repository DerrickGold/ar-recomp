/* The file:// editor loads classic scripts into one shared lexical scope.
 * Read the same load-order manifest as build.py, check that combined scope,
 * then return each diagnostic at its authored file/line. Nothing is exempt
 * from no-undef, and new scripts cannot silently miss the check. */
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const directory = path.dirname(fileURLToPath(import.meta.url));
const sources = [...fs.readFileSync(path.join(directory, "editor.body.html"), "utf8")
  .matchAll(/<script src="([a-z_]+\.js)"><\/script>/g)].map(match => match[1]);
if (!sources.length || new Set(sources).size !== sources.length) {
  throw new Error("Action editor script manifest is empty or repeats a script");
}
const ranges = new Map();

export default {
  preprocess(text, filename) {
    if (!sources.includes(path.basename(filename))) {
      throw new Error(`Action editor script is absent from its page manifest: ${filename}`);
    }
    let combined = "", start = 1;
    for (const source of sources) {
      const contents = source === path.basename(filename) ? text :
        fs.readFileSync(path.join(directory, source), "utf8");
      const lines = contents.split("\n").length;
      if (source === path.basename(filename)) ranges.set(filename, { start, end: start + lines });
      combined += contents + "\n";
      start += lines;
    }
    return [combined];
  },
  postprocess(messageLists, filename) {
    const { start, end } = ranges.get(filename);
    ranges.delete(filename);
    return messageLists.flat().filter(message => message.line >= start && message.line < end)
      .map(message => ({ ...message, line: message.line - start + 1,
        ...(message.endLine ? { endLine: message.endLine - start + 1 } : {}) }));
  }
};
