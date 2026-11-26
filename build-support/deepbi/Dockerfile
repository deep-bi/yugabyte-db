FROM almalinux:9 AS build

ARG BUILD_UID=1001
ARG BUILD_GID=1001
RUN dnf install -y epel-release dnf-plugins-core && \
    dnf config-manager --set-enabled crb && \
    dnf install -y \
      git gcc gcc-c++ make binutils bison flex perl gettext \
      pkgconf-pkg-config patch diffutils findutils file which hostname procps-ng \
      python3.11 python3.11-pip pam-devel openssl-devel glibc-langpack-en libatomic \
      rsync patchelf ninja-build golang curl-minimal ca-certificates tar gzip xz pigz \
      java-11-openjdk-devel maven time util-linux && \
    dnf clean all
RUN ln -s /usr/bin/python3.11 /usr/local/bin/python3 && \
    python3.11 -m pip install --no-cache-dir cmake==3.31.6 && \
    groupadd --gid "$BUILD_GID" ybci && \
    useradd --uid "$BUILD_UID" --gid "$BUILD_GID" --create-home ybci && \
    mkdir -p /opt/yb-build && chown ybci:ybci /opt/yb-build

ENV LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8 \
    JAVA_HOME=/usr/lib/jvm/java-11-openjdk \
    PATH=/home/ybci/.cargo/bin:/usr/local/bin:/usr/bin:/bin
COPY build.sh test.sh check_maven.sh check_reports.py /ci/
USER ybci
RUN curl -fsSL https://sh.rustup.rs -o /tmp/rustup.sh && \
    sh /tmp/rustup.sh -y --profile minimal && rm /tmp/rustup.sh
WORKDIR /src

FROM rockylinux:9 AS smoke

ARG BUILD_UID=1001
ARG BUILD_GID=1001
RUN dnf install -y \
      pam openssl-libs libatomic libxcrypt-compat glibc-langpack-en \
      python3 procps-ng findutils tar gzip shadow-utils util-linux time && \
    dnf clean all && \
    groupadd --gid "$BUILD_GID" ybci && \
    useradd --uid "$BUILD_UID" --gid "$BUILD_GID" --create-home ybci && \
    echo 'ybci:yb-ci-smoke-password' | chpasswd && \
    printf '%s\n' 'auth required pam_unix.so' 'account required pam_unix.so' \
      > /etc/pam.d/yb-ci
ENV LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8
COPY smoke.sh /ci/
USER ybci
WORKDIR /home/ybci
