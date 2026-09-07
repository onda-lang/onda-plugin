#pragma once

#include <juce_core/juce_core.h>

#include <filesystem>
#include <stdexcept>

// One unique, symlink-free root per test process, including on macOS where
// temp_directory_path() normally traverses the /var -> /private/var symlink.
inline const std::filesystem::path &testTemporaryRoot() {
  struct Root {
    std::filesystem::path path;

    Root() {
      const auto temporary =
          std::filesystem::canonical(std::filesystem::temp_directory_path());
      for (int attempt = 0; attempt < 100; ++attempt) {
        path = temporary /
               ("onda-plugin-test-" + juce::Uuid{}.toString().toStdString());
        if (std::filesystem::create_directory(path))
          return;
      }
      throw std::runtime_error("Could not create the test temporary directory");
    }

    ~Root() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  };
  static const Root root;
  return root.path;
}
