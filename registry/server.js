'use strict';

const express = require('express');
const path    = require('path');

const app  = express();
const PORT = 3000;

/* -----------------------------------------------------------------------
 * In-memory package registry.
 * downloadUrl points back to this server's /files/ static route so the
 * pkg client can fetch the tarball without a separate file host.
 * --------------------------------------------------------------------- */
const packages = [
  {
    name:          'hello',
    latestVersion: '1.0.0',
    downloadUrl:   'http://localhost:3000/files/hello-1.0.0.tar.gz',
    description:   'Minimal hello-world command demonstrating the pkg format',
  },
  {
    name:          'wcplus',
    latestVersion: '2.1.0',
    downloadUrl:   'http://localhost:3000/files/wcplus-2.1.0.tar.gz',
    description:   'Extended word-count utility with histogram support',
  },
  {
    name:          'aishell',
    latestVersion: '2.0',
    downloadUrl:   'http://localhost:3000/files/aishell-2.0.tar.gz',
    description:   'BusyBox-style shell with 32 built-in commands',
  },
];

/* -----------------------------------------------------------------------
 * Static file serving — place .tar.gz archives in ./files/
 * e.g.  registry/files/hello-1.0.0.tar.gz
 * --------------------------------------------------------------------- */
app.use('/files', express.static(path.join(__dirname, 'files')));

/* -----------------------------------------------------------------------
 * GET /packages
 * Returns the full registry as a JSON array.
 *
 * Example response:
 *   [
 *     { "name": "hello", "latestVersion": "1.0.0", "downloadUrl": "..." },
 *     { "name": "wcplus", "latestVersion": "2.1.0", "downloadUrl": "..." }
 *   ]
 * --------------------------------------------------------------------- */
app.get('/packages', (req, res) => {
  res.json(packages);
});

/* -----------------------------------------------------------------------
 * GET /packages/:name
 * Returns a single package object or a 404 JSON error.
 *
 * Example:  GET /packages/hello
 *   { "name": "hello", "latestVersion": "1.0.0", "downloadUrl": "..." }
 *
 * Example:  GET /packages/unknown
 *   404  { "error": "package 'unknown' not found" }
 * --------------------------------------------------------------------- */
app.get('/packages/:name', (req, res) => {
  const pkg = packages.find(p => p.name === req.params.name);
  if (!pkg) {
    return res.status(404).json({ error: `package '${req.params.name}' not found` });
  }
  res.json(pkg);
});

/* -----------------------------------------------------------------------
 * Start
 * --------------------------------------------------------------------- */
app.listen(PORT, () => {
  console.log(`aishell registry  →  http://localhost:${PORT}`);
  console.log(`  GET /packages`);
  console.log(`  GET /packages/:name`);
  console.log(`  GET /files/<archive>.tar.gz`);
});
