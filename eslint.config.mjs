import globals from "globals";
import editorScope from "./tools/action_editor/eslint_shared_scope.mjs";

export default [
  { ignores: ["**/node_modules/**", "**/vendor/**", "**/generated/**"] },
  {
    files: ["**/*.js", "**/*.mjs"],
    languageOptions: { ecmaVersion: "latest" },
    rules: {
      "no-undef": "error",
      "no-const-assign": "error",
      "no-import-assign": "error",
      "no-global-assign": "error",
      "no-dupe-args": "error",
      "no-dupe-keys": "error",
      "no-duplicate-case": "error",
      "no-unreachable": "error",
      "no-unsafe-finally": "error",
      "no-unsafe-optional-chaining": "error",
      "no-constant-binary-expression": "error",
      "no-sparse-arrays": "error",
      "valid-typeof": "error"
    }
  },
  {
    files: ["**/*.js", "installer/internal/builder/web/*.mjs"],
    languageOptions: { globals: globals.browser }
  },
  {
    files: ["**/*.test.mjs", "**/testdata/*.mjs", "eslint.config.mjs", "tools/**/*.mjs"],
    languageOptions: { globals: globals.node }
  },
  {
    files: ["tools/action_editor/*.js"],
    languageOptions: { sourceType: "script" },
    processor: editorScope
  }
];
