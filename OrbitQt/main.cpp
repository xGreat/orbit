// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <absl/flags/flag.h>
#include <absl/flags/parse.h>
#include <absl/flags/usage.h>
#include <absl/flags/usage_config.h>
#include <absl/strings/str_format.h>

#include <QAccessible>
#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QMessageBox>
#include <QMetaType>
#include <QObject>
#include <QPalette>
#include <QProcessEnvironment>
#include <QProgressDialog>
#include <QString>
#include <QStyleFactory>
#include <Qt>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <outcome.hpp>
#include <string>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "AccessibilityAdapter.h"
#include "Connections.h"
#include "DeploymentConfigurations.h"
#include "Error.h"
#include "ImGuiOrbit.h"
#include "OrbitBase/Logging.h"
#include "OrbitGgp/Error.h"
#include "OrbitSsh/Context.h"
#include "OrbitSsh/Credentials.h"
#include "OrbitSshQt/ScopedConnection.h"
#include "OrbitStartupWindow.h"
#include "OrbitVersion/OrbitVersion.h"
#include "Path.h"
#include "ProfilingTargetDialog.h"
#include "TargetConfiguration.h"
#include "opengldetect.h"
#include "orbitmainwindow.h"
#include "servicedeploymanager.h"

#ifdef ORBIT_CRASH_HANDLING
#include "CrashHandler.h"
#include "CrashOptions.h"
#endif

ABSL_DECLARE_FLAG(uint16_t, grpc_port);
ABSL_DECLARE_FLAG(std::string, collector_root_password);
ABSL_DECLARE_FLAG(std::string, collector);
ABSL_DECLARE_FLAG(bool, local);
ABSL_DECLARE_FLAG(bool, devmode);
ABSL_DECLARE_FLAG(bool, nodeploy);

using ServiceDeployManager = orbit_qt::ServiceDeployManager;
using DeploymentConfiguration = orbit_qt::DeploymentConfiguration;
using OrbitStartupWindow = orbit_qt::OrbitStartupWindow;
using Error = orbit_qt::Error;
using ScopedConnection = orbit_ssh_qt::ScopedConnection;
using GrpcPort = ServiceDeployManager::GrpcPort;
using SshCredentials = orbit_ssh::Credentials;
using Context = orbit_ssh::Context;

static outcome::result<GrpcPort> DeployOrbitService(
    std::optional<ServiceDeployManager>& service_deploy_manager,
    const DeploymentConfiguration* deployment_configuration, Context* context,
    const SshCredentials& ssh_credentials, const GrpcPort& remote_ports) {
  QProgressDialog progress_dialog{};

  service_deploy_manager.emplace(deployment_configuration, context, ssh_credentials, remote_ports);
  QObject::connect(&progress_dialog, &QProgressDialog::canceled, &service_deploy_manager.value(),
                   &ServiceDeployManager::Cancel);
  QObject::connect(&service_deploy_manager.value(), &ServiceDeployManager::statusMessage,
                   &progress_dialog, &QProgressDialog::setLabelText);
  QObject::connect(&service_deploy_manager.value(), &ServiceDeployManager::statusMessage,
                   [](const QString& msg) { LOG("Status message: %s", msg.toStdString()); });

  return service_deploy_manager->Exec();
}

static outcome::result<void> RunUiInstance(
    std::optional<DeploymentConfiguration> deployment_configuration, Context* context) {
  std::optional<orbit_qt::ServiceDeployManager> service_deploy_manager;

  OUTCOME_TRY(result, [&]() -> outcome::result<std::tuple<GrpcPort, QString>> {
    const GrpcPort remote_ports{/*.grpc_port =*/absl::GetFlag(FLAGS_grpc_port)};

    if (!deployment_configuration) {
      // When the local flag is present
      return std::make_tuple(remote_ports, QString{});
    }

    OrbitStartupWindow sw{};
    OUTCOME_TRY(result, sw.Run<orbit_ssh::Credentials>());

    if (!std::holds_alternative<orbit_ssh::Credentials>(result)) {
      // The user chose to open a capture.
      return std::make_tuple(remote_ports, std::get<QString>(result));
    }

    // The user chose a remote profiling target.
    OUTCOME_TRY(tunnel_ports,
                DeployOrbitService(service_deploy_manager, &(deployment_configuration.value()),
                                   context, std::get<SshCredentials>(result), remote_ports));
    return std::make_tuple(tunnel_ports, QString{});
  }());
  const auto& [ports, capture_path] = result;

  std::string grpc_server_address = absl::StrFormat("127.0.0.1:%d", ports.grpc_port);

  ServiceDeployManager* service_deploy_manager_ptr = nullptr;

  if (service_deploy_manager) {
    service_deploy_manager_ptr = &service_deploy_manager.value();
  }

  std::optional<std::error_code> error;

  orbit_qt::InstallAccessibilityFactories();
  {  // Scoping of QT UI Resources
    constexpr uint32_t kDefaultFontSize = 14;

    OrbitMainWindow w(service_deploy_manager_ptr, grpc_server_address, kDefaultFontSize);

    // "resize" is required to make "showMaximized" work properly.
    w.resize(1280, 720);
    w.showMaximized();

    auto error_handler = [&]() -> ScopedConnection {
      if (!service_deploy_manager) {
        return ScopedConnection();
      }

      return orbit_ssh_qt::ScopedConnection{QObject::connect(
          &service_deploy_manager.value(), &ServiceDeployManager::socketErrorOccurred,
          &service_deploy_manager.value(), [&](std::error_code e) {
            error = e;
            w.close();
            QApplication::quit();
          })};
    }();

    if (!capture_path.isEmpty()) {
      w.OpenCapture(capture_path.toStdString());
    }

    QApplication::exec();

    Orbit_ImGui_Shutdown();
  }

  if (error) {
    return outcome::failure(error.value());
  } else {
    return outcome::success();
  }
}

void RunBetaUiInstance(std::optional<DeploymentConfiguration> deployment_configuration,
                       const Context* ssh_context) {
  qRegisterMetaType<std::error_code>();

  // TODO(170468590): [ui beta] when out of ui beta, remove optional from deployment_configuration
  CHECK(deployment_configuration.has_value());

  const GrpcPort grpc_port{/*.grpc_port =*/absl::GetFlag(FLAGS_grpc_port)};

  orbit_qt::SshConnectionArtifacts ssh_connection_artifacts{ssh_context, grpc_port,
                                                            &(deployment_configuration.value())};

  std::optional<orbit_qt::TargetConfiguration> target_config;

  while (true) {
    {
      orbit_qt::ProfilingTargetDialog target_dialog{&ssh_connection_artifacts,
                                                    std::move(target_config)};
      target_config = target_dialog.Exec();

      if (!target_config.has_value()) {
        // User closed dialog
        break;
      }
    }

    int application_return_code = 0;

    {  // Scoping of QT UI Resources

      ServiceDeployManager* service_deploy_manager_ptr = nullptr;
      if (std::holds_alternative<orbit_qt::StadiaTarget>(target_config.value())) {
        service_deploy_manager_ptr = std::get<orbit_qt::StadiaTarget>(target_config.value())
                                         .GetConnection()
                                         ->GetServiceDeployManager();
      }

      constexpr uint32_t kDefaultFontSize = 14;

      OrbitMainWindow w(std::move(target_config.value()), kDefaultFontSize);

      // "resize" is required to make "showMaximized" work properly.
      w.resize(1280, 720);
      w.showMaximized();

      QMessageBox box(QMessageBox::Critical, QApplication::applicationName(), "", QMessageBox::Ok);
      auto error_handler = [&]() -> ScopedConnection {
        if (service_deploy_manager_ptr == nullptr) {
          return ScopedConnection();
        }

        return orbit_ssh_qt::ScopedConnection{QObject::connect(
            service_deploy_manager_ptr, &ServiceDeployManager::socketErrorOccurred, &box,
            [&](std::error_code error) {
              box.setText(
                  QString("Connection error: %1").arg(QString::fromStdString(error.message())));
              box.exec();
              w.close();
              w.ClearTargetConfiguration();
              QApplication::exit(OrbitMainWindow::kEndSessionReturnCode);
            })};
      }();

      application_return_code = QApplication::exec();

      target_config = w.ClearTargetConfiguration();
    }

    Orbit_ImGui_Shutdown();

    if (application_return_code == 0) {
      // User closed window
      break;
    } else if (application_return_code == OrbitMainWindow::kEndSessionReturnCode) {
      // User clicked end session, or socket error occurred.
      continue;
    } else {
      UNREACHABLE();
    }
  }
}

static void StyleOrbit(QApplication& app) {
  QApplication::setStyle(QStyleFactory::create("Fusion"));

  QPalette darkPalette;
  darkPalette.setColor(QPalette::Window, QColor(53, 53, 53));
  darkPalette.setColor(QPalette::WindowText, Qt::white);
  darkPalette.setColor(QPalette::Base, QColor(25, 25, 25));
  darkPalette.setColor(QPalette::AlternateBase, QColor(53, 53, 53));
  darkPalette.setColor(QPalette::ToolTipBase, Qt::white);
  darkPalette.setColor(QPalette::ToolTipText, Qt::white);
  darkPalette.setColor(QPalette::Text, Qt::white);
  darkPalette.setColor(QPalette::Button, QColor(53, 53, 53));
  darkPalette.setColor(QPalette::ButtonText, Qt::white);
  darkPalette.setColor(QPalette::BrightText, Qt::red);
  darkPalette.setColor(QPalette::Link, QColor(42, 130, 218));
  darkPalette.setColor(QPalette::Highlight, QColor(42, 130, 218));
  darkPalette.setColor(QPalette::HighlightedText, Qt::black);

  QColor light_gray{160, 160, 160};
  QColor dark_gray{90, 90, 90};
  QColor darker_gray{80, 80, 80};
  darkPalette.setColor(QPalette::Disabled, QPalette::Window, dark_gray);
  darkPalette.setColor(QPalette::Disabled, QPalette::WindowText, light_gray);
  darkPalette.setColor(QPalette::Disabled, QPalette::Base, darker_gray);
  darkPalette.setColor(QPalette::Disabled, QPalette::AlternateBase, dark_gray);
  darkPalette.setColor(QPalette::Disabled, QPalette::ToolTipBase, dark_gray);
  darkPalette.setColor(QPalette::Disabled, QPalette::ToolTipText, light_gray);
  darkPalette.setColor(QPalette::Disabled, QPalette::Text, light_gray);
  darkPalette.setColor(QPalette::Disabled, QPalette::Button, darker_gray);
  darkPalette.setColor(QPalette::Disabled, QPalette::ButtonText, light_gray);
  darkPalette.setColor(QPalette::Disabled, QPalette::BrightText, light_gray);
  darkPalette.setColor(QPalette::Disabled, QPalette::Link, light_gray);
  darkPalette.setColor(QPalette::Disabled, QPalette::Highlight, dark_gray);

  QApplication::setPalette(darkPalette);
  app.setStyleSheet(
      "QToolTip { color: #ffffff; background-color: #2a82da; border: 1px "
      "solid white; }");
}

static std::optional<std::string> GetCollectorRootPassword(
    const QProcessEnvironment& process_environment) {
  const char* const kEnvRootPassword = "ORBIT_COLLECTOR_ROOT_PASSWORD";
  if (FLAGS_collector_root_password.IsSpecifiedOnCommandLine()) {
    return absl::GetFlag(FLAGS_collector_root_password);
  } else if (process_environment.contains(kEnvRootPassword)) {
    return process_environment.value(kEnvRootPassword).toStdString();
  }
  return std::nullopt;
}

static std::optional<std::string> GetCollectorPath(const QProcessEnvironment& process_environment) {
  const char* const kEnvExecutablePath = "ORBIT_COLLECTOR_EXECUTABLE_PATH";
  if (FLAGS_collector.IsSpecifiedOnCommandLine()) {
    return absl::GetFlag(FLAGS_collector);
  } else if (process_environment.contains(kEnvExecutablePath)) {
    return process_environment.value(kEnvExecutablePath).toStdString();
  }
  return std::nullopt;
}

static std::optional<orbit_qt::DeploymentConfiguration> FigureOutDeploymentConfiguration() {
  if (absl::GetFlag(FLAGS_local) && !absl::GetFlag(FLAGS_enable_ui_beta)) {
    return std::nullopt;
  } else if (absl::GetFlag(FLAGS_nodeploy)) {
    return orbit_qt::NoDeployment{};
  }

  const char* const kEnvPackagePath = "ORBIT_COLLECTOR_PACKAGE_PATH";
  const char* const kEnvSignaturePath = "ORBIT_COLLECTOR_SIGNATURE_PATH";
  const char* const kEnvNoDeployment = "ORBIT_COLLECTOR_NO_DEPLOYMENT";

  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  std::optional<std::string> collector_path = GetCollectorPath(env);
  std::optional<std::string> collector_password = GetCollectorRootPassword(env);

  if (collector_path.has_value() && collector_password.has_value()) {
    return orbit_qt::BareExecutableAndRootPasswordDeployment{collector_path.value(),
                                                             collector_password.value()};
  } else if (env.contains(kEnvPackagePath) && env.contains(kEnvSignaturePath)) {
    return orbit_qt::SignedDebianPackageDeployment{env.value(kEnvPackagePath).toStdString(),
                                                   env.value(kEnvSignaturePath).toStdString()};
  } else if (env.contains(kEnvNoDeployment)) {
    return orbit_qt::NoDeployment{};
  } else {
    return orbit_qt::SignedDebianPackageDeployment{};
  }
}

static void DisplayErrorToUser(const QString& message) {
  QMessageBox::critical(nullptr, QApplication::applicationName(), message);
}

static bool DevModeEnabledViaEnvironmentVariable() {
  const auto env = QProcessEnvironment::systemEnvironment();
  return env.contains("ORBIT_DEV_MODE") || env.contains("ORBIT_DEVELOPER_MODE");
}

int main(int argc, char* argv[]) {
  // Will be filled by QApplication once instantiated.
  QString path_to_executable;

  // argv might be changed, so we make a copy here!
  auto original_argv = new char*[argc + 1];
  for (int i = 0; i < argc; ++i) {
    const auto size = std::strlen(argv[i]);
    auto dest = new char[size];
    std::strncpy(dest, argv[i], size);
    original_argv[i] = dest;
  }
  original_argv[argc] = nullptr;

  {
    absl::SetProgramUsageMessage("CPU Profiler");
    absl::SetFlagsUsageConfig(absl::FlagsUsageConfig{{}, {}, {}, &orbit_core::GetBuildReport, {}});
    absl::ParseCommandLine(argc, argv);

    InitLogFile(Path::GetLogFilePathAndCreateDir());
    LOG("You are running Orbit Profiler version %s", orbit_core::GetVersion());

#if __linux__
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
#endif

    QApplication app(argc, argv);
    QApplication::setOrganizationName("The Orbit Authors");
    QApplication::setApplicationName("orbitprofiler");

    if (DevModeEnabledViaEnvironmentVariable()) {
      absl::SetFlag(&FLAGS_devmode, true);
    }

    // The application display name is automatically appended to all window titles when shown in the
    // title bar: <specific window title> - <application display name>
    const auto version_string = QString::fromStdString(orbit_core::GetVersion());
    auto display_name = QString{"Orbit Profiler %1 [BETA]"}.arg(version_string);

    if (absl::GetFlag(FLAGS_devmode)) {
      display_name.append(" [DEVELOPER MODE]");
    }

    QApplication::setApplicationDisplayName(display_name);
    QApplication::setApplicationVersion(version_string);
    path_to_executable = QCoreApplication::applicationFilePath();

#ifdef ORBIT_CRASH_HANDLING
    const std::string dump_path = Path::CreateOrGetDumpDir().string();
#ifdef _WIN32
    const char* handler_name = "crashpad_handler.exe";
#else
    const char* handler_name = "crashpad_handler";
#endif
    const std::string handler_path =
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(handler_name).toStdString();
    const std::string crash_server_url = CrashServerOptions::GetUrl();
    const std::vector<std::string> attachments = {Path::GetLogFilePathAndCreateDir().string()};

    CrashHandler crash_handler(dump_path, handler_path, crash_server_url, attachments);
#endif  // ORBIT_CRASH_HANDLING

    StyleOrbit(app);

    const auto deployment_configuration = FigureOutDeploymentConfiguration();

    const auto open_gl_version = orbit_qt::DetectOpenGlVersion();

    if (!open_gl_version) {
      DisplayErrorToUser(
          "OpenGL support was not found. Please make sure you're not trying to "
          "start Orbit in a remote session and make sure you have a recent "
          "graphics driver installed. Then try again!");
      return -1;
    }

    LOG("Detected OpenGL version: %i.%i", open_gl_version->major, open_gl_version->minor);

    if (open_gl_version->major < 2) {
      DisplayErrorToUser(QString("The minimum required version of OpenGL is 2.0. But this machine "
                                 "only supports up to version %1.%2. Please make sure you're not "
                                 "trying to start Orbit in a remote session and make sure you "
                                 "have a recent graphics driver installed. Then try again!")
                             .arg(open_gl_version->major)
                             .arg(open_gl_version->minor));
      return -1;
    }

    auto context = Context::Create();
    if (!context) {
      DisplayErrorToUser(QString("An error occurred while initializing ssh: %1")
                             .arg(QString::fromStdString(context.error().message())));
      return -1;
    }

    if (absl::GetFlag(FLAGS_enable_ui_beta)) {
      RunBetaUiInstance(deployment_configuration, &context.value());
      return 0;
    } else {
      while (true) {
        const auto result = RunUiInstance(deployment_configuration, &(context.value()));
        if (result || result.error() == make_error_code(Error::kUserClosedStartUpWindow) ||
            !deployment_configuration) {
          // It was either a clean shutdown or the deliberately closed the
          // dialog, or we started with the --local flag.
          return 0;
        } else if (result.error() == make_error_code(orbit_ggp::Error::kCouldNotUseGgpCli)) {
          DisplayErrorToUser(QString::fromStdString(result.error().message()));
          return 1;
        } else if (result.error() != make_error_code(Error::kUserCanceledServiceDeployment)) {
          DisplayErrorToUser(QString("An error occurred: %1")
                                 .arg(QString::fromStdString(result.error().message())));
          break;
        }
      }
    }
  }

  execv(path_to_executable.toLocal8Bit().constData(), original_argv);
  UNREACHABLE();
}
