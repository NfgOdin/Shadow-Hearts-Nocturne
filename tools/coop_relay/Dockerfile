FROM python:3.12-slim
WORKDIR /app
COPY coop_relay.py /app/coop_relay.py
EXPOSE 47191/udp
CMD ["python3", "-u", "/app/coop_relay.py", "--host", "0.0.0.0", "--port", "47191"]
