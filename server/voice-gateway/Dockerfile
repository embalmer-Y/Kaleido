FROM python:3.12-slim
WORKDIR /app
RUN pip install --no-cache-dir websockets
COPY gateway.py .
EXPOSE 9100
CMD ["python", "gateway.py"]
