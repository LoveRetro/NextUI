# Contributing to NextUI

Thank you for your interest in contributing to NextUI!

## Code Formatting

NextUI uses [clang-format](https://clang.llvm.org/docs/ClangFormat.html) to maintain consistent code formatting across all C/C++ source files.

### Setup

Clang-format is likely already installed on most development systems. You can verify by running:

```bash
clang-format --version
```

If not installed, you can install it:

- **Ubuntu/Debian**: `sudo apt-get install clang-format`
- **macOS**: `brew install clang-format`
- **Windows**: Download from [LLVM releases](https://releases.llvm.org/)

### Formatting Your Code

Before submitting a pull request, please format your code using clang-format:

```bash
# Format a single file
clang-format -i path/to/your/file.c

# Format all C/C++ files in the workspace (excluding _unmaintained)
find workspace -type f \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) | grep -v "_unmaintained" | xargs clang-format -i
```

### Checking Formatting

To check if your files are properly formatted without modifying them:

```bash
clang-format --dry-run --Werror path/to/your/file.c
```

### Configuration

The clang-format configuration is defined in `.clang-format` at the root of the repository. Key style guidelines:

- **Indentation**: Tabs (4 spaces wide)
- **Brace Style**: Linux style (opening brace on next line for functions)
- **Line Length**: 120 characters maximum
- **Pointer Alignment**: Right-aligned (`char *ptr` not `char* ptr`)
- **Space After Keywords**: `if (condition)` not `if(condition)`

### Editor Integration

Most modern editors and IDEs support clang-format integration:

- **VS Code**: Install the "C/C++" extension
- **Vim**: Use `vim-clang-format` plugin
- **Emacs**: Use `clang-format.el`
- **CLion/IntelliJ**: Built-in support

This ensures your code is automatically formatted as you write it.

## Pull Request Guidelines

1. Format your code using clang-format before submitting
2. Ensure your changes don't break existing functionality
3. Write clear commit messages
4. Update documentation if needed

## Questions?

If you have any questions about contributing, feel free to open an issue or join our [Discord](https://discord.gg/HKd7wqZk3h).
