# Instruções do Projeto — Ext4Windows

## Perfil do Desenvolvedor
O Mateus vem de Python e está aprendendo C++ construindo este projeto. Ele NÃO tem experiência prévia com C/C++.

## Como Explicar Mudanças
Toda mudança no código DEVE ser explicada em detalhes, como se o Mateus não soubesse nada de C/C++. Especificamente:

1. **Cada palavra-chave nova** deve ser explicada (extern, static, struct, void*, #pragma, #include, etc.)
2. **Cada decisão de design** deve ter o motivo explicado
3. **Sempre incluir links** de documentação oficial para cada conceito:
   - C++: https://en.cppreference.com/
   - Windows API: https://learn.microsoft.com/
   - WinFsp: https://winfsp.dev/
   - lwext4: https://github.com/gkostka/lwext4
   - CMake: https://cmake.org/cmake/help/latest/
4. **Comparar com Python** quando possível para facilitar o entendimento
5. **Ir por partes**, passo a passo, sem pular etapas
6. **Não poupar palavras** — quanto mais contexto, melhor

## Exemplo de Explicação Adequada
Errado: "Criei uma função static que retorna void*"
Certo: "Criei uma função `static` (que significa que ela só é visível dentro deste arquivo — em Python seria como uma função que começa com `_`, indicando uso interno). Ela retorna `void*`, que é um ponteiro genérico — imagine como se fosse retornar `Any` em Python, pode apontar para qualquer tipo de dado. [Link: https://en.cppreference.com/w/cpp/language/static] [Link: https://en.cppreference.com/w/c/language/pointer]"

## Stack do Projeto
- **C++17** — linguagem principal
- **CMake** — sistema de build (equivalente ao setup.py/pyproject.toml do Python)
- **WinFsp** — framework FUSE para Windows (cria drives virtuais)
- **lwext4** — biblioteca C que lê/escreve o formato ext4
- **Qt 6** — GUI (futuro)
