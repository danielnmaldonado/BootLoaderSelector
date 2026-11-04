# Validação da versão 2.32

Créditos: ©DNMTechLabs / dnmtechlabs@gmail.com

Correções da base v2.31/Round86 verificadas em 2026-11-03. O ZIP original foi
preservado; o pacote corrigido distribui fontes, avisos de licença e catálogo
para uma dependência de compilação externa, sem os binários antigos.

## Matriz local

Ambiente: macOS Intel x86-64, Apple Clang 21, GNU GCC 15.3, Python 3.14,
OpenSSL 4.1 e Jsign 7.5. O build usa a toolchain cruzada externa original,
cuja lista de 951 arquivos e respectivos hashes é conferida antes de executar.

| Verificação | Resultado |
| --- | --- |
| Gate com `BLS_QUALITY_REPEAT=3`, incluindo build | Aprovado |
| Clang ASan/UBSan | 18 testes × 3 execuções |
| GNU GCC UBSan | 18 testes × 3 execuções |
| Clang ASan/IntegerSan | 18 testes × 1 execução |
| Clang ThreadSanitizer | 18 testes × 1 execução |
| BaseTools deep/bounds/fuzz | 2 testes por compilador |
| Compilação estrita e análise Clang/GNU GCC | 9 módulos C próprios por compilador |
| Testes BaseTools/Python durante o build | 285 aprovados |
| Certificado positivo pela API PEM/DER | 2 testes aprovados |
| Política de certificado, bootstrap e assinatura PE/COFF | Aprovados |
| Publicação, rollback, lock e cancelamentos | Aprovados; 9 cenários HUP/INT/TERM |
| BaseTools sob GNU GCC com fortificação e `-O2 -Werror` | Compilação e seis casos válidos de mensagens aprovados; compilador selecionado respeitado |
| GenFv/GenFw sob diagnóstico de retornos de stdio | Compilação GNU/Clang e conversões válidas de EFI/HII/FV/cápsula aprovadas |
| Workflow GitHub Actions | Sintaxe/semântica conferidas com actionlint; ações oficiais fixadas por commit |
| Boot UEFI isolado e chainload de child que retorna sucesso | Log confirma início, chamada e retorno `call=0x0 child=0x0` |

O novo teste usa a DevicePathLib real: 26 conversões válidas e 76 posições de
falha de alocação por execução, com verificação explícita da posse das alocações.
Não se limita ao stub de conversão usado pela suíte antiga. A revisão
independente não deixou achados acionáveis pendentes nos caminhos corrigidos.

## Recursos indisponíveis e alcance

LeakSanitizer do Clang, ASan do GNU GCC e ThreadSanitizer do GNU GCC não são
utilizáveis neste host. Os probes benignos detectam e registram `SKIP`; não são
contados como cobertura aprovada. O Clang mantém ASan sem detecção de leaks e o
GNU executa UBSan. O CI Linux exige ASan com detecção de leaks e não usa essa
redução de matriz do macOS.

O smoke local usa uma imagem de disco nova, OVMF com hashes conferidos e o QEMU
já instalado no Android SDK. O log do guest confirma o chainload; o wrapper
Android encerra com sinal durante o teardown e pode fechar QMP antes da consulta
final. Isso não é evidência de estabilidade da interface gráfica. O CI usa QEMU
nativo Linux e o par CODE/VARS 4M fornecido pelo Ubuntu 24.04.

Nenhum teste instala o programa na EFI do host ou altera sua NVRAM. Boot em
hardware real, confiança Secure Boot e integração com OpenCore/Clover reais
continuam sem homologação. A assinatura ad-hoc não cria confiança no firmware.

## Reprodução e evidência

Em um checkout limpo, com as dependências descritas no README:

```sh
BLS_QUALITY_REPEAT=3 ./AuditTests/Run-Release-Quality-Gate.command
python3 AuditTools/Update-Source-Manifest.py
```

No macOS, indique `BLS_TOOLCHAIN_DIR` e exponha `lld-link`/OpenSSL no PATH.
A distribuição pode ser compilada novamente no Linux sem a toolchain antiga.
A assinatura usa uma chave efêmera nova; portanto os EFI assinados podem ter
hashes diferentes mesmo com o mesmo payload. Confira a proveniência e os
certificados de cada build, em vez de exigir identidade entre assinaturas novas.

Os resultados remotos do código efetivamente enviado ficam na aba Actions do
repositório privado. Tags cuja versão corresponde aos fontes publicam uma
pré-release somente após os checks. A configuração de Sponsors está preparada;
a ativação do perfil e dos recebimentos depende do cadastro do titular no GitHub.
