# Contributing to Samsung SDI Victron Integration

Thank you for your interest in contributing to the Samsung SDI Victron Integration project! This document provides guidelines and information for contributors.

## 🚀 Getting Started

### Prerequisites
- Python 3.7+
- Victron Venus OS (for testing) or Linux development environment
- CAN interface (for hardware testing)
- Git

### Development Setup

1. **Clone the repository**
   ```bash
   git clone https://github.com/drurew/samsung-sdi-victron-integration.git
   cd samsung-sdi-victron-integration
   ```

2. **Set up development environment**
   ```bash
   # Install Python dependencies
   pip3 install python-can dbus-python pygobject3

   # For testing without Venus OS
   pip3 install pytest mock
   ```

3. **Run tests**
   ```bash
   python3 -m pytest test_samsung_sdi.py -v
   ```

## 🛠️ Development Guidelines

### Code Style
- Follow PEP 8 Python style guidelines
- Use type hints for function parameters and return values
- Write docstrings for all classes and public methods
- Use meaningful variable and function names
- Keep functions small and focused on single responsibilities

### Commit Messages
Use clear, descriptive commit messages following this format:
```
type(scope): description

[optional body]

[optional footer]
```

Types:
- `feat`: New feature
- `fix`: Bug fix
- `docs`: Documentation changes
- `style`: Code style changes
- `refactor`: Code refactoring
- `test`: Test additions/modifications
- `chore`: Maintenance tasks

Examples:
```
feat(can): add support for extended CAN message parsing
fix(dbus): resolve service registration timeout issue
docs(install): update installation instructions for Venus OS v3.0
```

### Branch Naming
- `main`: Production-ready code
- `develop`: Development branch
- `feature/feature-name`: New features
- `bugfix/bug-description`: Bug fixes
- `hotfix/critical-fix`: Critical fixes for production

## 📝 Making Changes

### 1. Choose an Issue
- Check existing [GitHub Issues](https://github.com/drurew/samsung-sdi-victron-integration/issues)
- Create a new issue if your change isn't already tracked
- Comment on the issue to indicate you're working on it

### 2. Create a Branch
```bash
git checkout -b feature/your-feature-name
# or
git checkout -b bugfix/your-bug-fix
```

### 3. Make Changes
- Write clean, well-documented code
- Add tests for new functionality
- Update documentation as needed
- Test your changes thoroughly

### 4. Test Your Changes

**Unit Tests:**
```bash
python3 -m pytest test_samsung_sdi.py -v
```

**Integration Testing:**
```bash
# Test CAN client
python3 -c "from samsung_sdi_can_client import SamsungSDICANClient; print('Import successful')"

# Test service (without D-Bus)
python3 samsung_sdi_bms_service.py --test-mode
```

**Hardware Testing (on Venus OS):**
```bash
# Run diagnostics
python3 diagnose_charging.py

# Monitor CAN traffic
candump can0 | grep "500\|501\|502"
```

### 5. Update Documentation
- Update README.md for new features
- Update INSTALL.md for installation changes
- Update TROUBLESHOOTING.md for new issues/solutions
- Add code comments for complex logic

### 6. Commit Changes
```bash
git add .
git commit -m "feat: add your feature description"
```

### 7. Push and Create Pull Request
```bash
git push origin feature/your-feature-name
```
Then create a Pull Request on GitHub.

## 🧪 Testing

### Test Categories
- **Unit Tests**: Test individual functions and classes
- **Integration Tests**: Test component interactions
- **Hardware Tests**: Test with real Samsung SDI hardware
- **Venus OS Tests**: Test on actual Venus OS environment

### Writing Tests
```python
import pytest
from samsung_sdi_can_client import SamsungSDICANClient

def test_can_message_parsing():
    """Test CAN message parsing functionality"""
    # Test code here
    assert True

def test_voltage_scaling():
    """Test voltage value scaling"""
    # Test voltage conversion
    assert True
```

### Test Coverage
Aim for high test coverage, especially for:
- CAN message parsing logic
- D-Bus service registration
- Battery aggregation algorithms
- Error handling paths

## 🐛 Reporting Bugs

### Bug Report Template
When reporting bugs, please include:

1. **Description**: Clear description of the issue
2. **Steps to Reproduce**: Step-by-step instructions
3. **Expected Behavior**: What should happen
4. **Actual Behavior**: What actually happens
5. **Environment**:
   - Venus OS version
   - Python version
   - Hardware setup
   - CAN configuration
6. **Logs**: Relevant log output
7. **Diagnostic Output**: Output from `diagnose_charging.py`

### Debugging Tips
```bash
# Enable debug logging
python3 samsung_sdi_bms_service.py --debug

# Monitor CAN traffic
candump can0

# Check D-Bus services
dbus-send --system --dest=org.freedesktop.DBus --type=method_call --print-reply /org/freedesktop/DBus org.freedesktop.DBus.ListNames | grep samsung
```

## 📚 Documentation

### Code Documentation
- Use docstrings for all public functions/classes
- Include type hints
- Document parameters, return values, and exceptions

### User Documentation
- Keep README.md up to date
- Update INSTALL.md for installation changes
- Add troubleshooting guides for common issues
- Include examples and screenshots where helpful

## 🔒 Security Considerations

- Never commit sensitive information (passwords, keys, personal data)
- Be careful with CAN bus access - ensure proper permissions
- Validate all input data to prevent injection attacks
- Follow principle of least privilege

## 🤝 Code Review Process

### Review Checklist
- [ ] Code follows style guidelines
- [ ] Tests pass and coverage is adequate
- [ ] Documentation is updated
- [ ] No sensitive information committed
- [ ] Commit messages are clear and descriptive
- [ ] Breaking changes are documented

### Review Comments
- Be constructive and specific
- Suggest improvements, don't just point out problems
- Explain reasoning for requested changes
- Acknowledge good practices

## 📞 Getting Help

- **GitHub Issues**: For bugs and feature requests
- **GitHub Discussions**: For questions and general discussion
- **Victron Community**: For Victron-specific questions

## 🎉 Recognition

Contributors will be recognized in:
- Repository contributors list
- Changelog entries
- Release notes

Thank you for contributing to the Samsung SDI Victron Integration project! 🚀