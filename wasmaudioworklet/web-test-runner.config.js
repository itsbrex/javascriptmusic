import { playwrightLauncher } from '@web/test-runner-playwright';

export default {
  files: [
    '**/*.spec.js', // include `.spec.ts` files
    '!./node_modules/**/*', // exclude any node modules
  ],
  concurrency: 1,
  watch: false,
  testRunnerHtml: testRunnerImport =>
    `<html>
      <body>
        <script type="module">
            import { expect, assert} from 'https://cdn.jsdelivr.net/npm/chai@5.0.0/+esm';
            globalThis.assert = assert;
            globalThis.expect = expect;
        </script>        
        <script type="module" src="${testRunnerImport}"></script>
      </body>
    </html>`,
  browsers: [
    playwrightLauncher({ product: 'chromium', launchOptions: { args: ['--autoplay-policy=no-user-gesture-required'] } }),
    playwrightLauncher({
      product: 'firefox', launchOptions: {
        headless: false,
        firefoxUserPrefs: {
          'media.autoplay.block-webaudio': false,  // Allow Web Audio autoplay
          'media.autoplay.default': 0,            // Allow autoplay for all media
          'media.autoplay.allow-extension-background-pages': true,
          'media.autoplay.blocking_policy': 0,
          'dom.require_user_interaction_for_audio': false, // Remove user gesture requirement
          'dom.audiochannel.mutedByDefault': false
        }
      }
    }),
    /*playwrightLauncher({
      product: 'webkit',launchOptions: {
        headless: false
      }
    })*/
  ],
};