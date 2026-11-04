# BootLoaderSelector

Seletor UEFI x64 para localizar e iniciar bootloaders, com seleção lembrada,
configuração em plist, informações de hardware e logs.

Créditos: ©DNMTechLabs / dnmtechlabs@gmail.com

## Compilação

Este repositório distribui fontes e testes. Compilar gera os arquivos em `Release/`;
o script não instala nada na partição EFI e não configura o firmware.

No Linux x86-64, instale GCC/G++, binutils, make, NASM, Python 3.10+, Clang/LLD,
OpenSSL e sbsigntools. Em Ubuntu:

```sh
sudo apt-get install gcc g++ binutils make nasm python3 clang lld openssl sbsigntool
./Build-BootLoaderSelector-Release.command
```

No macOS, instale Xcode Command Line Tools, Python 3.10+, Java, Jsign e OpenSSL (não o LibreSSL do sistema).
Por exemplo, `brew install jsign openssl@3`; coloque o `bin` de OpenSSL no PATH. A compilação
UEFI também precisa da toolchain cruzada original, fornecida separadamente por
quem já a possui. Ela não acompanha esta distribuição. Veja
[Toolchains/README.md](Toolchains/README.md).

```sh
BLS_TOOLCHAIN_DIR=/caminho/sem/espacos/toolchain.gcc102.snb \
  ./Build-BootLoaderSelector-Release.command
```

A toolchain externa é conferida contra um catálogo de hashes antes da execução.
Não use uma ferramenta apenas porque tem o mesmo nome. Para obter um build sem
essa dependência antiga de macOS, use o ambiente Linux acima.

Os dois EFI de saída têm conteúdo idêntico: `BootLoaderSelector.efi` e
`BOOTX64.efi`. A assinatura ad-hoc comprova a integridade relativa ao certificado
gerado; ela não torna o arquivo automaticamente confiável para Secure Boot.

## Testes

```sh
BLS_QUALITY_BUILD=0 ./AuditTests/Run-Release-Quality-Gate.command
```

O gate executa testes de runtime e BaseTools, regressões de assinatura/publicação
e análises estáticas. Para os testes de assinatura, `lld-link` deve estar no PATH. No Homebrew,
instale `lld@21` separadamente e exponha `$(brew --prefix lld@21)/bin`;
o pacote `llvm@21` sozinho não fornece esse comando.
Clang e GNU GCC são identificados pela implementação, não apenas pelo nome.
Recursos de sanitizer indisponíveis são informados como `SKIP`, não como aprovados.
O CI exige os gates Linux e macOS antes de compilar os artefatos de entrega e
exercita o chainload de um child dedicado em OVMF.

O manifesto cobre os arquivos de fonte, documentação e CI. Alterações legítimas
precisam de um novo manifesto: após revisar suas mudanças, execute
`python3 AuditTools/Update-Source-Manifest.py --write`; sem `--write`, a ferramenta
apenas verifica o estado. O builder rejeita divergências e arquivos extras;
saídas de build, metadados Git e `.DS_Store` são exceções documentadas.

Resultados concretos e limites de homologação: [VALIDATION.md](VALIDATION.md).

## Licenças

O código próprio usa [BSD-3-Clause](LICENSE), conforme escolha do autor.
Isso não substitui as licenças de `MdePkg`, `BaseTools` e demais terceiros.
Veja [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt) e `LICENSES/`.

Chaves de teste upstream em BaseTools são fixtures públicas, identificadas em
[SECURITY-NOTES.txt](SECURITY-NOTES.txt). Não são credenciais de produção.

## Actions e releases

Pushes em `main`, pull requests e tags `v*` executam as verificações. Uma tag
que corresponda à versão dos fontes publica uma pré-release somente depois dos
checks Linux/macOS, assinatura e OVMF. Os ZIPs de fontes/EFI e seus SHA-256 são
anexados à release; os EFI incluem os avisos de licença. A visibilidade dos
artefatos e releases segue a visibilidade privada do repositório.

O arquivo `.github/FUNDING.yml` aponta para `danielnmaldonado`. O perfil GitHub
Sponsors ainda precisa da ativação pelo titular; o arquivo não ativa pagamentos.
