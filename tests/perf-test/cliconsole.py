# -*- coding: utf-8 -*-
import os
import sys
import logging
import threading
from logging.handlers import TimedRotatingFileHandler
import traceback
import coloredlogs
import click
import configparser
from perfpeasant import Peasant
from enums.DataScaleEnum import DataScaleEnum
from util.shellutil import CommandRunner
import schedule
import time


# 初始化日志处理
def initLogger(loggerName: str):
    # 初始化读取配置文件实例
    confile = os.path.join(os.path.dirname(__file__), "conf", "config.ini")
    cf = configparser.ConfigParser()
    cf.read(confile, encoding='UTF-8')

    perf_test_path = cf.get("machineconfig", "perf_test_path")

    # 开启日志记录
    logger = logging.getLogger(loggerName)

    # 设置程序的日志级别, 并按照coloredlogs的要求标记各种图形化颜色显示
    os.environ["COLOREDLOGS_LEVEL_STYLES"] = \
        "spam=22;debug=28;verbose=34;notice=220;warning=202;success=118,bold;" \
        "error=background=red,bold;critical=background=red"
    LOG_FORMAT = "%(asctime)s - %(levelname)9s - %(message)s"
    logFormat = logging.Formatter(LOG_FORMAT)

    file_handler = logging.FileHandler(perf_test_path + '/perf_test.log')
    file_handler.setFormatter(logFormat)
    logger.addHandler(file_handler)

    # 创建一个 handler，用于写入日志文件
    # midnight: 表示日志文件在每天半夜时分滚动
    # interval: 间隔时间单位的个数，指等待多少个 when 的时间后 Logger 会自动重建新闻继续进行日志记录
    # backupCount: 表示日志文件的保留个数，假如为7，则会保留最近的7个日志文件
    save_handler = TimedRotatingFileHandler("perf_test.log", when="midnight", interval=1, backupCount=7)
    save_handler.suffix = "%Y-%m-%d"  # 设置日志文件名的时间戳格式

    logger.addHandler(save_handler)

    consoleLogHandler = logging.StreamHandler()
    consoleLogHandler.setFormatter(logFormat)

    # 默认日志输出级别是INFO级别
    consoleLogHandler.setLevel(logging.INFO)
    coloredlogs.install(
        level=consoleLogHandler.level,
        fmt=LOG_FORMAT,
        logger=logger,
        isatty=True
    )
    return logger


def clean_task(logger):
    # 初始化读取配置文件实例
    confile = os.path.join(os.path.dirname(__file__), "conf", "config.ini")
    cf = configparser.ConfigParser()
    cf.read(confile, encoding='UTF-8')
    perf_test_path = cf.get("machineconfig", "perf_test_path")

    schedule.every().day.at("23:59").do(clean_files, logger=logger, perf_test_path=perf_test_path)

    while True:
        schedule.run_pending()
        time.sleep(1)

def clean_files(logger, perf_test_path):


    cmdHandler = CommandRunner(logger)
    cmdHandler.run_command(path=perf_test_path,
                           command="find {0}/test_case_* -type d -mtime +2 -exec rm -rf {} \\".format(perf_test_path))

@click.group(invoke_without_command=True)
@click.pass_context
@click.option("--version", is_flag=True, help="Show Farm version.")
def cli(ctx, version):
    if not ctx.invoked_subcommand:
        # 打印版本信息
        if version:
            import pkg_resources
            try:
                click.secho("Version: " + pkg_resources.get_distribution("farm").version)
            except pkg_resources.DistributionNotFound:
                click.secho("Version: 1.0.0.0")
            return
        click.echo(ctx.get_help())


# @cli.command(help="基于用户指定版本运行性能测试")
# @click.option("--version", "-v", type=str, required=True, help="指定的产品版本号，用逗号分隔.", )
# @click.option("--logfile", "-l", type=str, help="指定文件的日志位置.", )
# def run_PerfTest(
#         branch,
#         logfile
# ):
#     pass

@cli.command(help="启动一个后台服务，在服务中根据用户输入的分支信息，循环执行性能测试")
@click.option("--branches", "-b", type=str, required=True, help="指定分支信息，用逗号分隔", )
# @click.option("--data-scale", "-s", type=str, required=False, default="mid", help="性能测试数据规模，暂支持3个级别：big、mid和tiny", )
@click.option("--test-group", "-g", type=str, required=True, help="测试组文件名", )
@click.option("--machine", "-m", type=str, required=True, help="测试机器集群id", )
def run_PerfTest_Backend(
        branches: str,
        # data_scale: str,
        test_group: str,
        machine: str
):
    # 初始化配置文件读取实例
    confile = os.path.join(os.path.dirname(__file__), "conf", "config.ini")
    cf = configparser.ConfigParser()
    cf.read(confile, encoding='UTF-8')
    perf_test_path = cf.get("machineconfig", "perf_test_path")

    # 初始化工作目录，若不存在，创建工作目录
    if not os.path.exists(os.path.join(perf_test_path)):
        os.makedirs(os.path.join(perf_test_path))

    # 初始化logger
    appLogger = initLogger("Performance_testing")

    # 开始运行性能测试
    appLogger.info("")
    appLogger.info("【开始执行性能测试】")
    appLogger.info(
        f'性能测试命令：cliconsole.py --branches {branches} --test-group {test_group} --machine {machine}')

    # 解析branch参数
    # 参数校验，若输入参数中有不存在的分支，直接退出
    # 获取github上对应repo的所有分支
    # github_repo = "{0}/{1}".format(cf.get("github", "namespace"), cf.get("github", "project"))
    # github = GitHubUtil(github_repo)
    # current_branches = github.get_branches()

    # branch_list = branches.split(',')
    # for branch in branch_list:
    #     if branch == "main":
    #         continue
    #     if branch not in current_branches:
    #         appLogger.error('输入分支不存在，分支名称：{0}'.format(branch))
    #         exit(1)

    # 参数校验，若输入参数中有符合的数据规模参数，直接退出
    # perf_test_scale = None
    # if data_scale.lower() == DataScaleEnum.tinyweight.value:
    #     perf_test_scale = DataScaleEnum.tinyweight
    # elif data_scale.lower() == DataScaleEnum.midweight.value:
    #     perf_test_scale = DataScaleEnum.midweight
    # elif data_scale.lower() == DataScaleEnum.bigweight.value:
    #     perf_test_scale = DataScaleEnum.bigweight
    # else:
    #     appLogger.error("输入的数据规模格式不对，正确格式：[big、mid、tiny]，实际输入：{0}".format(data_scale))
    #     sys.exit(-1)

    # 在启动性能测试前，创建守护子进程对即将保存的日志文件进行定期清理
    clean_backup_file_thread = threading.Thread(name="clean_backup_file_thread", target=clean_task,
                                                kwargs={"logger": appLogger})
    clean_backup_file_thread.daemon = True

    clean_backup_file_thread.start()
    appLogger.warning("启动子进程对备份的日志进行周期性清理")

    branch_list = branches.split(',')
    # 无限轮询
    while True:
        # 循环执行性能测试
        # while True:
        # 轮询每个配置的分支，运行一次性能测试
        for branch in branch_list:
            # 初始化性能执行器
            perfTester = Peasant(logger=appLogger)

            # 配置分支
            perfTester.set_branch(branch=branch)

            # 配置测试环境
            perfTester.set_machine(cluster_id=machine)

            # 配置数据量级
            perfTester.set_test_group(test_group=test_group)

            # ###################
            # 清理环境
            perfTester.clean_env()

            # 下载db源代码
            perfTester.download_db()

            # 判断将要运行测试用例的分支最新commit是否有更新，若是数据库中存储的最新commit_id不等于当前的commit_id，则执行性能测试
            # if perfTester.is_last_commit(branch=branch):
            #     appLogger.warning("分支代码没有更新，sleep 5s")
            #     time.sleep(5)
            #     continue

            # 安装db
            perfTester.install_db()

            # 插入数据
            perfTester.insert_data()

            # 执行查询
            perfTester.run_test_case()

            # 备份数据
            perfTester.backup_test_case()
            # ###################


@cli.command(help="关闭后台运行性能测试的服务，会确保正在运行的测试完成后才会停止服务")
@click.option("--wait-for-finished", type=str, default=True, help="是否等待正在运行的性能测试完成后在停止服务.默认为True", )
def abort_PerfTest_Backend(
        wait_for_finished
):
    print(wait_for_finished)
    pass


if __name__ == "__main__":

    # run_PerfTest_Backend("main,3.0,3.1")
    # # 禁用paramiko的一些不必要日志
    # logging.getLogger("paramiko").setLevel(logging.WARNING)

    # 运行应用程序
    try:
        cli()
        sys.exit(0)
    except Exception as ge:
        print('traceback.print_exc():\n%s' % traceback.print_exc())
        print('traceback.format_exc():\n%s' % traceback.format_exc())
        click.secho(repr(ge), err=True, fg="red")
        sys.exit(1)

    pass
