
```
██████╗  █████╗ ███████╗███╗   ███╗
██╔══██╗██╔══██╗██╔════╝████╗ ████║
██████╔╝███████║███████╗██╔████╔██║
██╔══██╗██╔══██║╚════██║██║╚██╔╝██║
██║  ██║██║  ██║███████║██║ ╚═╝ ██║
╚═╝  ╚═╝╚═╝  ╚═╝╚══════╝╚═╝     ╚═╝
                                   
Rendering Engine
```

RASM (Arabic: رسم) is an open-source rendering engine currently in early development, Designed for modern GPUs, RASM aims to deliver high performance and low driver overhead.

It provides maximum flexibility by offering both a convenient high-level API for quick development and a low-level API for granular control.


## Documentation

The docs are HTML pages in [`docs/`](./docs). Clone the repo and open [`docs/index.html`](./docs/index.html) in a browser. They need no server and no build, and they work offline.

| Docs | For | What is inside |
|---|---|---|
| [Guides](./docs/guides) | engine developers | How RASM works inside, explained with diagrams, one topic per page: the frame loop, how data reaches the GPU, the GPU scene tables, the uber shader, GPU culling, the render graph, the stats overlay, and more. |
| [API reference](./docs/api) | application developers | Getting started, then every public type and function, with examples. |
| [architecture.md](./docs/architecture.md) | both | The full technical reference of the engine as it is today. |

## Development
To build RASM, check the [`build-guide`](./docs/build_guide.md) 

## License

RASM is licensed under [MIT license](/LICENSE).