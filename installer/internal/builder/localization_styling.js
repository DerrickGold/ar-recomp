(() => {
  "use strict";

  // This edits source ranges, not rendered text. The Go parser remains the
  // authority for validation. Keep syntax tokens intact and add an override
  // inside existing tags so applying Upright to italic text actually wins.
  function editableRanges(source) {
    const ranges = [], atoms = [];
    function text(start, end, atomic = false) {
      if (ranges.at(-1)?.end === start) ranges.at(-1).end = end;
      else ranges.push({start, end});
      if (atomic) atoms.push({start, end});
    }
    let skipUntil = 0;
    for (const line of source.matchAll(/[^\r\n]+/g)) {
      const offset = line.index, value = line[0], leading = value.search(/[^ \t]/);
      // Blank source lines are paragraph controls, even when indented.
      if (leading < 0) continue;
      const content = value.slice(leading);
      if (/^(?:@(?!@)|#|;|::)/.test(content)) continue;
      let at = Math.max(0, skipUntil - offset);
      // These escapes are interpreted at the start of a source line. Moving
      // a tag ahead of them would change their meaning, so keep them in place.
      if (!at && /^(?:@@|\\[#;])/.test(content)) {
        atoms.push({start: offset + leading, end: offset + leading + 2});
        at = leading + 2;
      }
      while (at < value.length) {
        const start = at, char = value[at];
        if (char === "<") {
          const close = source.indexOf(">", offset + at + 1);
          skipUntil = close < 0 ? source.length : close + 1;
          at = skipUntil - offset;
          atoms.push({start: offset + start, end: offset + at});
        } else if (char === "|") {
          at++;
        } else if (char === "\\" || value.startsWith("{{", at) || value.startsWith("}}", at)) {
          at = Math.min(at + 2, value.length);
          text(offset + start, offset + at, true);
        } else if (char === "{") {
          const close = value.indexOf("}", at + 1);
          at = close < 0 ? value.length : close + 1;
          text(offset + start, offset + at, true);
        } else {
          at += value.codePointAt(at) > 0xffff ? 2 : 1;
          text(offset + start, offset + at);
        }
      }
    }
    return {ranges, atoms};
  }

  function wrap(source, start, end, open, close) {
    const {ranges, atoms} = editableRanges(source);
    if (atoms.some(token => start > token.start && start < token.end ||
                            end > token.start && end < token.end))
      throw new Error("selection");
    // Never split a combining sequence, surrogate pair or joined emoji.
    const graphemes = new Intl.Segmenter(undefined, {granularity: "grapheme"});
    for (const part of graphemes.segment(source)) {
      const last = part.index + part.segment.length;
      if (start > part.index && start < last || end > part.index && end < last)
        throw new Error("selection");
    }
    if (start === end) {
      const lineStart = source.lastIndexOf("\n", start - 1) + 1;
      const nextLine = source.indexOf("\n", start);
      const line = source.slice(lineStart, nextLine < 0 ? source.length : nextLine);
      if (line.trim() && !ranges.some(range => start >= range.start && start <= range.end))
        throw new Error("selection");
      return {text: open + close, start, end, selectStart: start + open.length, selectEnd: start + open.length};
    }
    const pieces = ranges.map(range => ({start: Math.max(start, range.start), end: Math.min(end, range.end)}))
      .filter(range => range.start < range.end);
    if (!pieces.length) throw new Error("selection");
    let text = "", at = start, selectStart, selectEnd;
    for (const piece of pieces) {
      text += source.slice(at, piece.start) + open;
      selectStart ??= start + text.length;
      text += source.slice(piece.start, piece.end);
      selectEnd = start + text.length;
      text += close;
      at = piece.end;
    }
    text += source.slice(at, end);
    return {text, start, end, selectStart, selectEnd};
  }

  window.workshopInlineStyle = {wrap};
})();
