# Dependências de compilação

A distribuição pública não inclui executáveis da antiga `toolchain.gcc102.snb`.
A origem e os fontes exatos dessa distribuição binária não foram estabelecidos;
por isso seus binários não são redistribuídos neste repositório ou no ZIP de fontes.

## Linux

O builder usa GCC, G++, gcc-ar, objcopy e NASM instalados no host. O catálogo da
toolchain de macOS não é executado nem necessário nesse caminho. Os hashes das
ferramentas utilizadas são registrados na proveniência da compilação.

## macOS

Quem já possui a toolchain do pacote original v2.31/Round86 pode mantê-la fora
do repositório e indicar seu diretório com `BLS_TOOLCHAIN_DIR`. Use um caminho
sem espaços, sem links simbólicos e com as permissões de execução originais.

O catálogo `gcc102-snb.sha256` registra os 951 arquivos originais. O builder
confere o catálogo, a lista exata de arquivos e cada hash antes de executar
qualquer ferramenta dessa dependência. Não baixa toolchains automaticamente.
Apple Silicon também depende da compatibilidade Rosetta com esses binários Intel.

Esse catálogo identifica bytes; ele não comprova procedência, autoria ou direito
de redistribuição. Para builds públicos independentes dessa ferramenta antiga,
use o caminho Linux. Não acrescente os binários à árvore versionada.

O signer Jsign é uma dependência separada. O helper pode obter o JAR oficial 7.5,
com hash fixado, em cache externo ao projeto; `BLS_JSIGN_AUTO_DOWNLOAD=0` desativa
esse bootstrap. `BLS_JSIGN_CACHE_DIR` permite escolher o cache.
