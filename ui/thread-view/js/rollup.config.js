import babel from 'rollup-plugin-babel'
import commonjs from 'rollup-plugin-commonjs'
import nodeResolve from 'rollup-plugin-node-resolve'
import replace from 'rollup-plugin-replace'
import uglify from 'rollup-plugin-uglify'
import livereload from 'rollup-plugin-livereload'
import serve from 'rollup-plugin-serve'
import path from 'path'
import alias from 'rollup-plugin-alias'
import eslint from 'rollup-plugin-eslint'

const production = !process.env.ROLLUP_WATCH

export default {
  watch: {
    chokidar: true,
    include: 'src/*'
  },
  globals: {
    // react: ['React', 'Component', 'createElement']
  },
  plugins: [

    eslint(),
    process.env.NODE_ENV && replace({ 'process.env.NODE_ENV': JSON.stringify(process.env.NODE_ENV) }),
    alias({
      'react': path.resolve(__dirname, 'node_modules', 'preact-compat', 'dist', 'preact-compat.es.js'),
      'react-dom': path.resolve(__dirname, 'node_modules', 'preact-compat', 'dist', 'preact-compat.es.js'),
      'create-react-class': path.resolve(__dirname, 'node_modules', 'preact-compat', 'lib', 'create-react-class.js')
    }),

    babel({
      exclude: [
        'node_modules/!(' +
        'google-map-react|preact|preact-compat|react-redux' +
        ')/**'
      ]
    }),

    nodeResolve(),

    commonjs({
      include: 'node_modules/**',
      namedExports: {
        'node_modules/react-dom/index.js': [
          'render'
        ],
        /*
        "node_modules/react/react.js": [
          "Component",
          "createElement"
        ],
        */
        'node_modules/preact/src/preact.js': [
          'h',
          'createElement',
          'cloneElement',
          'Component',
          'render',
          'rerender',
          'options'
        ],
        'node_modules/kefir/dist/kefir.js': [
          'Observable',
          'Property',
          'Stream',
          'combine',
          'concat',
          'constant',
          'fromEvents',
          'interval',
          'later',
          'merge',
          'never'
        ]
      }
    }),

    process.env.NODE_ENV === 'production' && uglify({
      compress: {
        passes: 3
      }
    }),

    !production && serve('dist'),
    !production && livereload('dist')

  ].filter(x => x)
}
