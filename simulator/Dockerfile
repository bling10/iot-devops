FROM python:3.13.5-alpine3.22

WORKDIR /app

COPY requirements.txt /app/requirements.txt
RUN pip install --no-cache-dir --requirement /app/requirements.txt \
    && addgroup -S -g 10001 simulator \
    && adduser -S -D -H -u 10001 -G simulator simulator \
    && mkdir -p /app/state \
    && chown simulator:simulator /app/state

COPY . /app/simulator

USER simulator

ENV PYTHONDONTWRITEBYTECODE=1

ENTRYPOINT ["python", "-m", "simulator.main"]
