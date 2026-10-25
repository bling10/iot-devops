FROM jenkins/jenkins:2.567-jdk21

USER root

RUN apt-get update \
    && apt-get install -y --no-install-recommends python3 \
    && rm -rf /var/lib/apt/lists/*

USER jenkins
