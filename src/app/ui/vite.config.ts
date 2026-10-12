import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// Served by the app from memory under https://bridger.local/ (src/app/ui_bundle.cpp).
export default defineConfig({
  plugins: [react()],
  base: './',
  build: {
    target: 'es2022',
    assetsInlineLimit: 0,
    modulePreload: { polyfill: false },
    reportCompressedSize: false,
  },
})
